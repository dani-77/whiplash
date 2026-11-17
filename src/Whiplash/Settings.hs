-- | Turns ~/.config/whiplash/config.toml into a 'Config', on top of the
-- defaults compiled into the binary. Every key the file sets replaces the
-- default; a [keybinds] table replaces the whole default binding set, so
-- the file always shows every active binding. Unknown keys are errors, so
-- a typo never goes unnoticed.
module Whiplash.Settings
  ( loadSettings
  , builtinLayouts
  , actionNames
  ) where

import Control.Monad (foldM, when)
import Data.Bits ((.|.))
import Data.Char (isHexDigit, isSpace, ord, toLower)
import Data.List (intercalate)
import qualified Data.Map.Strict as M
import Data.Word (Word32)
import Numeric (readHex)
import Text.Read (readMaybe)

import Whiplash.Core
import qualified Whiplash.Heart as H
import Whiplash.Keys
import Whiplash.Layout
import qualified Whiplash.StackSet as W
import Whiplash.Toml

builtinLayouts :: [Layout]
builtinLayouts = [tall, wide, full]

-- | Parse TOML text and apply it to the defaults.
loadSettings :: Config -> String -> IO (Either String Config)
loadSettings base src = case parseToml src of
  Left err -> pure (Left err)
  Right table -> applySettings base table

type Result a = Either String a

applySettings :: Config -> Table -> IO (Result Config)
applySettings base t = case pure base >>= checkKeys "" topKeys t >>= plain of
  Left err -> pure (Left err)
  Right cfg -> case M.lookup "keybinds" t of
    Nothing -> pure (Right cfg)
    Just (VTable binds) -> fmap (\ks -> cfg {keys = const ks}) <$> bindings cfg binds
    Just v -> pure (Left ("keybinds must be a table, not " ++ typeName v))
  where
    topKeys =
      [ "terminal", "mod", "gaps", "border", "layouts", "workspaces"
      , "keyboard", "autostart", "keybinds"
      ]
    plain cfg =
      pure cfg
        >>= field "terminal" str (\v c -> c {terminal = v})
        >>= field "mod" modifier (\v c -> c {modMask = v})
        >>= field "gaps" (nat "gaps") (\v c -> c {gaps = v})
        >>= field "border" border id
        >>= field "layouts" layoutList (\v c -> c {layouts = v})
        >>= field "workspaces" workspaces (\v c -> c {workspaceTags = v})
        >>= field "keyboard" (keyboardTable (keyboard cfg)) (\v c -> c {keyboard = v})
        >>= field "autostart" (strList "autostart") (\v c -> c {startupHook = mapM_ spawn v})
    field :: String -> (Value -> Result a) -> (a -> Config -> Config) -> Config -> Result Config
    field k decode set c = case M.lookup k t of
      Nothing -> Right c
      Just v -> either (Left . ((k ++ ": ") ++)) (\a -> Right (set a c)) (decode v)

    border (VTable b) = do
      _ <- checkKeys "border." ["width", "focused", "normal"] b ()
      w <- maybe (Right Nothing) (fmap Just . nat "width") (M.lookup "width" b)
      f <- maybe (Right Nothing) (fmap Just . color) (M.lookup "focused" b)
      n <- maybe (Right Nothing) (fmap Just . color) (M.lookup "normal" b)
      Right $ \c ->
        c
          { borderWidth = maybe (borderWidth c) id w
          , focusedBorderColor = maybe (focusedBorderColor c) id f
          , normalBorderColor = maybe (normalBorderColor c) id n
          }
    border v = Left ("expected a table, got " ++ typeName v)

checkKeys :: String -> [String] -> Table -> a -> Result a
checkKeys prefix allowed t a = case filter (`notElem` allowed) (M.keys t) of
  [] -> Right a
  bad ->
    Left ("unknown setting " ++ intercalate ", " (map (prefix ++) bad)
      ++ " (known: " ++ intercalate ", " allowed ++ ")")

-- Scalars ----------------------------------------------------------------

str :: Value -> Result String
str (VString s) = Right s
str v = Left ("expected a string, got " ++ typeName v)

nat :: String -> Value -> Result Int
nat _ (VInt n) | n >= 0 && n < 100000 = Right (fromIntegral n)
nat what (VInt n) = Left (what ++ " out of range: " ++ show n)
nat _ v = Left ("expected a whole number, got " ++ typeName v)

strList :: String -> Value -> Result [String]
strList _ (VArray vs) = mapM str vs
strList _ v = Left ("expected an array of strings, got " ++ typeName v)

-- | "#rrggbb" or "#rrggbbaa".
color :: Value -> Result Word32
color (VString ('#' : hex))
  | length hex `elem` [6, 8] && all isHexDigit hex =
      case readHex (if length hex == 6 then hex ++ "ff" else hex) of
        [(n, "")] -> Right n
        _ -> Left ("bad colour #" ++ hex)
color (VString s) = Left ("colours are written \"#rrggbb\" or \"#rrggbbaa\", not " ++ show s)
color v = Left ("expected a colour string, got " ++ typeName v)

modifier :: Value -> Result KeyMask
modifier v = str v >>= \s -> case lookupMod (map toLower s) of
  Just m -> Right m
  Nothing -> Left ("unknown modifier " ++ show s ++ " (use Alt, Super, Ctrl or Shift)")

lookupMod :: String -> Maybe KeyMask
lookupMod m
  | m `elem` ["alt", "mod1"] = Just mod1Mask
  | m `elem` ["super", "logo", "win", "mod4"] = Just mod4Mask
  | m `elem` ["ctrl", "control"] = Just controlMask
  | m == "shift" = Just shiftMask
  | otherwise = Nothing

layoutList :: Value -> Result [Layout]
layoutList v = do
  names <- strList "layouts" v
  when (null names) $ Left "needs at least one layout"
  mapM pick names
  where
    known = [(map toLower (layoutName l), l) | l <- builtinLayouts]
    pick n = maybe (Left ("unknown layout " ++ show n ++ " (known: "
                           ++ intercalate ", " (map fst known) ++ ")"))
                   Right (lookup (map toLower n) known)

-- | A workspace count (1-9); changes only take effect on restart.
workspaces :: Value -> Result [Int]
workspaces (VInt n) | n >= 1 && n <= 9 = Right [1 .. fromIntegral n]
workspaces v = Left ("expected a number from 1 to 9, got " ++ showValue v)

keyboardTable :: KeyboardSettings -> Value -> Result KeyboardSettings
keyboardTable kb (VTable k) = do
  _ <- checkKeys "keyboard." ["layout", "variant", "options", "repeat_rate", "repeat_delay"] k ()
  let get name def decode = maybe (Right def) decode (M.lookup name k)
  l <- get "layout" (kbLayout kb) str
  v <- get "variant" (kbVariant kb) str
  o <- get "options" (kbOptions kb) str
  r <- get "repeat_rate" (kbRepeatRate kb) (nat "repeat_rate")
  d <- get "repeat_delay" (kbRepeatDelay kb) (nat "repeat_delay")
  Right (KeyboardSettings l v o r d)
keyboardTable _ v = Left ("expected a table, got " ++ typeName v)

showValue :: Value -> String
showValue (VInt n) = show n
showValue v = typeName v

-- Key bindings -----------------------------------------------------------

bindings :: Config -> Table -> IO (Result [((KeyMask, KeySym), X ())])
bindings cfg table = fmap sequence (mapM one (M.toList table))
  where
    one (combo, v) = do
      k <- parseCombo (modMask cfg) combo
      pure $ either (Left . (("keybinds." ++ show combo ++ ": ") ++)) Right $ do
        key <- k
        actionText <- str v
        act <- parseAction cfg actionText
        Right (key, act)

-- | "Mod+Shift+Return" -> (mask, keysym). "Mod" is the configured modifier.
parseCombo :: KeyMask -> String -> IO (Result (KeyMask, KeySym))
parseCombo modKey combo = case splitOn '+' combo of
  parts | null parts || any null parts -> pure (Left "bad key combination")
  parts -> do
    let (mods, keyName) = (init parts, last parts)
        maskE = foldM addMod 0 mods
    sym <- keysym keyName
    pure $ do
      mask <- maskE
      s <- maybe (Left ("unknown key " ++ show keyName)) Right sym
      Right (mask, s)
  where
    addMod acc m = case map toLower m of
      "mod" -> Right (acc .|. modKey)
      other -> maybe (Left ("unknown modifier " ++ show m)) (Right . (acc .|.)) (lookupMod other)

-- | Single characters are their own keysym (letters lowercased, since
-- bindings see unshifted keys); anything longer is an xkb key name.
keysym :: String -> IO (Maybe KeySym)
keysym [c]
  | c >= 'A' && c <= 'Z' = pure (Just (fromIntegral (ord (toLower c))))
  | ord c < 0x7f = pure (Just (fromIntegral (ord c)))
keysym name = H.keysymFromName name

splitOn :: Char -> String -> [String]
splitOn c s = case break (== c) s of
  (a, []) -> [a]
  (a, _ : b) -> a : splitOn c b

-- | The words a binding can say. Kept next to 'parseAction'.
actionNames :: [String]
actionNames =
  [ "spawn <command>", "close", "quit", "reload"
  , "focus-down", "focus-up", "swap-down", "swap-up", "swap-master"
  , "workspace <n>", "move-to-workspace <n>", "workspace-previous"
  , "layout <name>", "next-layout", "master-ratio <+/-delta>"
  , "toggle-float", "fullscreen"
  ]

parseAction :: Config -> String -> Result (X ())
parseAction cfg text = case words text of
  ("spawn" : _) -> case dropWhile isSpace (drop 5 (dropWhile isSpace text)) of
    "" -> Left "spawn needs a command"
    cmd -> Right (spawn cmd)
  ["close"] -> Right kill
  ["quit"] -> Right quit
  ["reload"] -> Right reload
  ["focus-down"] -> Right (windows W.focusDown)
  ["focus-up"] -> Right (windows W.focusUp)
  ["swap-down"] -> Right (windows W.swapDown)
  ["swap-up"] -> Right (windows W.swapUp)
  ["swap-master"] -> Right (windows W.swapMaster)
  ["workspace-previous"] -> Right (windows W.toggleWS)
  ["workspace", n] -> W.view <$> tag n >>= Right . windows
  ["move-to-workspace", n] -> W.shift <$> tag n >>= Right . windows
  ["next-layout"] -> Right nextLayout
  ["layout", name]
    | map toLower name `elem` map (map toLower . layoutName) (layouts cfg) ->
        Right (jumpToLayout name)
    | otherwise -> Left ("layout " ++ show name ++ " is not in the layouts list")
  ["master-ratio", d] -> case readMaybe (dropWhile (== '+') d) :: Maybe Double of
    Just x | abs x < 1 -> Right (incRatio x)
    _ -> Left ("master-ratio wants a step like +0.05 or -0.05, not " ++ show d)
  ["toggle-float"] -> Right toggleFloat
  ["fullscreen"] -> Right toggleFullscreen
  _ -> Left ("unknown action " ++ show text ++ " (known: " ++ intercalate ", " actionNames ++ ")")
  where
    tag n = case readMaybe n of
      Just t | t `elem` workspaceTags cfg -> Right t
      _ -> Left ("no workspace " ++ show n)
