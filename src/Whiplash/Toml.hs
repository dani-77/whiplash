-- | A small TOML reader covering what a whiplash config uses: bare, quoted
-- and dotted keys, [tables], basic and literal strings, integers (also
-- 0x hex), floats, booleans, arrays (spanning lines, trailing comma) and
-- inline tables. Multi-line strings, dates and [[arrays of tables]] are
-- rejected with a clear error rather than misread.
--
-- Written by hand because no TOML library ships with GHC and whiplash
-- builds offline from boot packages only.
module Whiplash.Toml
  ( Value (..)
  , Table
  , parseToml
  , typeName
  ) where

import Data.Char (chr, isAlphaNum, isDigit, isHexDigit, isSpace)
import qualified Data.Map.Strict as M
import Numeric (readHex)

data Value
  = VString String
  | VInt Integer
  | VFloat Double
  | VBool Bool
  | VArray [Value]
  | VTable Table
  deriving (Show, Eq)

type Table = M.Map String Value

hexValue :: String -> Int
hexValue h = case readHex h of
  [(n, "")] -> n
  _ -> 0

typeName :: Value -> String
typeName v = case v of
  VString _ -> "a string"
  VInt _ -> "an integer"
  VFloat _ -> "a number"
  VBool _ -> "a boolean"
  VArray _ -> "an array"
  VTable _ -> "a table"

-- | Input still to read, and the current line (for error messages).
data St = St {rest :: String, line :: !Int}

type P a = St -> Either String (a, St)

failAt :: St -> String -> Either String b
failAt st msg = Left ("line " ++ show (line st) ++ ": " ++ msg)

parseToml :: String -> Either String Table
parseToml src = go [] M.empty (St src 1)
  where
    go path acc st0 = do
      let st = skipBlank st0
      case rest st of
        [] -> Right acc
        '[' : '[' : _ -> failAt st "arrays of tables ([[...]]) are not supported"
        '[' : _ -> do
          (k, st') <- key (skipWs (advance 1 st))
          st'' <- expect ']' (skipWs st')
          st3 <- endOfLine st''
          acc' <- ensureTable st k acc
          go k acc' st3
        _ -> do
          (k, st1) <- key st
          st2 <- expect '=' (skipWs st1)
          (v, st3) <- value (skipWs st2)
          st4 <- endOfLine st3
          acc' <- insertAt st (path ++ k) v acc
          go path acc' st4

-- Tables -----------------------------------------------------------------

insertAt :: St -> [String] -> Value -> Table -> Either String Table
insertAt st [] _ _ = failAt st "empty key"
insertAt st [k] v t
  | M.member k t = failAt st ("key '" ++ k ++ "' is defined twice")
  | otherwise = Right (M.insert k v t)
insertAt st (k : ks) v t = case M.lookup k t of
  Nothing -> (\sub -> M.insert k (VTable sub) t) <$> insertAt st ks v M.empty
  Just (VTable sub) -> (\sub' -> M.insert k (VTable sub') t) <$> insertAt st ks v sub
  Just _ -> failAt st ("'" ++ k ++ "' is not a table")

ensureTable :: St -> [String] -> Table -> Either String Table
ensureTable _ [] t = Right t
ensureTable st (k : ks) t = case M.lookup k t of
  Nothing -> (\sub -> M.insert k (VTable sub) t) <$> ensureTable st ks M.empty
  Just (VTable sub) -> (\sub' -> M.insert k (VTable sub') t) <$> ensureTable st ks sub
  Just _ -> failAt st ("'" ++ k ++ "' is not a table")

-- Lexical helpers --------------------------------------------------------

advance :: Int -> St -> St
advance n st =
  let (taken, left) = splitAt n (rest st)
   in St left (line st + length (filter (== '\n') taken))

-- | Spaces and tabs only.
skipWs :: St -> St
skipWs st = case rest st of
  (c : _) | c == ' ' || c == '\t' -> skipWs (advance 1 st)
  _ -> st

-- | Whitespace, newlines and comments.
skipBlank :: St -> St
skipBlank st = case rest st of
  (c : _) | isSpace c -> skipBlank (advance 1 st)
  ('#' : _) -> skipBlank (skipComment st)
  _ -> st

skipComment :: St -> St
skipComment st = St (dropWhile (/= '\n') (rest st)) (line st)

endOfLine :: St -> Either String St
endOfLine st0 =
  let st = skipWs st0
   in case rest st of
        [] -> Right st
        ('\n' : _) -> Right (advance 1 st)
        ('\r' : '\n' : _) -> Right (advance 2 st)
        ('#' : _) -> Right (skipComment st)
        (c : _) -> failAt st ("unexpected '" ++ [c] ++ "' after the value")

expect :: Char -> St -> Either String St
expect c st = case rest st of
  (x : _) | x == c -> Right (advance 1 st)
  _ -> failAt st ("expected '" ++ [c] ++ "'")

-- Keys -------------------------------------------------------------------

key :: P [String]
key st = do
  (k, st1) <- keyPart st
  let st2 = skipWs st1
  case rest st2 of
    ('.' : _) -> do
      (ks, st3) <- key (skipWs (advance 1 st2))
      Right (k : ks, st3)
    _ -> Right ([k], st1)

keyPart :: P String
keyPart st = case rest st of
  ('"' : _) -> basicString (advance 1 st)
  ('\'' : _) -> literalString (advance 1 st)
  _ ->
    let (k, _) = span bareChar (rest st)
     in if null k then failAt st "expected a key" else Right (k, advance (length k) st)
  where
    bareChar c = isAlphaNum c || c == '_' || c == '-'

-- Values -----------------------------------------------------------------

value :: P Value
value st = case rest st of
  ('"' : '"' : '"' : _) -> failAt st "multi-line strings are not supported"
  ('\'' : '\'' : '\'' : _) -> failAt st "multi-line strings are not supported"
  ('"' : _) -> first VString <$> basicString (advance 1 st)
  ('\'' : _) -> first VString <$> literalString (advance 1 st)
  ('[' : _) -> array (advance 1 st)
  ('{' : _) -> inlineTable (advance 1 st)
  ('t' : 'r' : 'u' : 'e' : _) -> Right (VBool True, advance 4 st)
  ('f' : 'a' : 'l' : 's' : 'e' : _) -> Right (VBool False, advance 5 st)
  (c : _) | isDigit c || c == '+' || c == '-' -> number st
  _ -> failAt st "expected a value"
  where
    first f (a, s) = (f a, s)

basicString :: P String
basicString = go []
  where
    go acc st = case rest st of
      [] -> failAt st "unterminated string"
      ('\n' : _) -> failAt st "unterminated string"
      ('"' : _) -> Right (reverse acc, advance 1 st)
      ('\\' : e : _) -> case e of
        'n' -> go ('\n' : acc) (advance 2 st)
        't' -> go ('\t' : acc) (advance 2 st)
        'r' -> go ('\r' : acc) (advance 2 st)
        'b' -> go ('\b' : acc) (advance 2 st)
        'f' -> go ('\f' : acc) (advance 2 st)
        '"' -> go ('"' : acc) (advance 2 st)
        '\\' -> go ('\\' : acc) (advance 2 st)
        'u' -> unicode 4 acc st
        'U' -> unicode 8 acc st
        _ -> failAt st ("unknown escape \\" ++ [e])
      (c : _) -> go (c : acc) (advance 1 st)
    unicode n acc st =
      let digits = take n (drop 2 (rest st))
       in if length digits == n && all isHexDigit digits
            then go (chr (hexValue digits) : acc) (advance (2 + n) st)
            else failAt st "bad unicode escape"

literalString :: P String
literalString st =
  let (s, r) = break (\c -> c == '\'' || c == '\n') (rest st)
   in case r of
        ('\'' : _) -> Right (s, advance (length s + 1) st)
        _ -> failAt st "unterminated string"

number :: P Value
number st =
  let (tok, _) = span (\c -> isAlphaNum c || c `elem` "+-._") (rest st)
      clean = filter (/= '_') tok
      st' = advance (length tok) st
      unsigned = dropWhile (`elem` "+-") clean
      neg = take 1 clean == "-"
      sign x = if neg then negate x else x
   in case unsigned of
        ('0' : 'x' : hex)
          | not (null hex) && all isHexDigit hex ->
              Right (VInt (sign (fromIntegral (hexValue hex))), st')
        _
          | not (null unsigned) && all isDigit unsigned ->
              Right (VInt (sign (read unsigned)), st')
          | otherwise -> case reads (fixFloat unsigned) of
              [(d, "")] -> Right (VFloat (sign d), st')
              _ -> failAt st ("bad number '" ++ tok ++ "'")
  where
    -- Haskell's read wants a digit on both sides of the point.
    fixFloat s = case break (== '.') s of
      (a, '.' : b) -> (if null a then "0" else a) ++ "." ++ (case b of
        (d : _) | isDigit d -> b
        _ -> '0' : b)
      _ -> s

array :: P Value
array = go []
  where
    go acc st0 =
      let st = skipBlank st0
       in case rest st of
            (']' : _) -> Right (VArray (reverse acc), advance 1 st)
            _ -> do
              (v, st1) <- value st
              let st2 = skipBlank st1
              case rest st2 of
                (',' : _) -> go (v : acc) (advance 1 st2)
                (']' : _) -> Right (VArray (reverse (v : acc)), advance 1 st2)
                _ -> failAt st2 "expected ',' or ']' in array"

inlineTable :: P Value
inlineTable st0 =
  let st = skipWs st0
   in case rest st of
        ('}' : _) -> Right (VTable M.empty, advance 1 st)
        _ -> go M.empty st
  where
    go acc st = do
      (k, st1) <- key (skipWs st)
      st2 <- expect '=' (skipWs st1)
      (v, st3) <- value (skipWs st2)
      acc' <- insertAt st k v acc
      let st4 = skipWs st3
      case rest st4 of
        (',' : _) -> go acc' (advance 1 st4)
        ('}' : _) -> Right (VTable acc', advance 1 st4)
        _ -> failAt st4 "expected ',' or '}' in inline table"
