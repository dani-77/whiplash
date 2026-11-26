-- | Turns ~/.config/whiplash/config.toml into a 'Config', on top of the
-- defaults compiled into the binary. Every key the file sets replaces the
-- default. A [keybinds] table replaces the whole default binding set.
-- Backend tables can override autostart and individual bindings.
module Whiplash.Settings
  ( loadSettings
  , loadSettingsFor
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
loadSettings = loadSettingsFor H.Wayland

-- | Apply the shared settings, then select the overrides for this backend.
loadSettingsFor :: H.Backend -> Config -> String -> IO (Either String Config)
loadSettingsFor backend base src = case parseToml src of
  Left err -> pure (Left err)
  Right table -> do
    shared <- applySettings base table
    case shared of
      Left err -> pure (Left err)
      Right cfg -> do
        x11 <- applyProfile "x11" cfg table
        wayland <- applyProfile "wayland" cfg table
        pure $ do
          x11Config <- x11
          waylandConfig <- wayland
          pure (if backend == H.X11 then x11Config else waylandConfig)

type Result a = Either String a

applySettings :: Config -> Table -> IO (Result Config)
applySettings base t = case pure base >>= checkKeys "" topKeys t >>= plain >>= rules >>= swipes of
  Left err -> pure (Left err)
  Right cfg -> case M.lookup "keybinds" t of
    Nothing -> pure (Right cfg)
    Just (VTable binds) -> fmap (\ks -> cfg {keys = const ks}) <$> bindings cfg binds
    Just v -> pure (Left ("keybinds must be a table, not " ++ typeName v))
  where
    topKeys =
      [ "terminal", "mod", "gaps", "border", "layouts", "workspaces", "focus_follows_mouse"
      , "keyboard", "touchpad", "autostart", "keybinds", "outputs", "rules", "scratchpads", "gestures"
      , "x11", "wayland"
      ]
    plain cfg =
      pure cfg
        >>= field "terminal" str (\v c -> c {terminal = v})
        >>= field "mod" modifier (\v c -> c {modMask = v})
        >>= field "gaps" (nat "gaps") (\v c -> c {gaps = v})
        >>= field "scratchpads" scratchpadTable (\v c -> c {scratchpads = v})
        >>= field "focus_follows_mouse" bool (\v c -> c {focusFollowsMouse = v})
        >>= field "border" border id
        >>= field "layouts" layoutList (\v c -> c {layouts = v})
        >>= field "workspaces" workspaces (\v c -> c {workspaceTags = v})
        >>= field "keyboard" (keyboardTable (keyboard cfg)) (\v c -> c {keyboard = v})
        >>= field "touchpad" (touchpadTable (touchpad cfg)) (\v c -> c {touchpad = v})
        >>= field "autostart" (strList "autostart") (\v c -> c {startupHook = mapM_ spawn v})
        >>= field "outputs" outputTable (\v c -> c {outputRules = v})
    -- After the plain fields: a rule's workspace must exist.
    rules cfg = field "rules" (ruleTable (workspaceTags cfg)) (\v c -> c {windowRules = v}) cfg
    -- Also after the plain fields: actions refer to workspaces and slots.
    swipes cfg = field "gestures" (gestureTable cfg) (\v c -> c {gestures = v}) cfg
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

applyProfile :: String -> Config -> Table -> IO (Result Config)
applyProfile name cfg root = case M.lookup name root of
  Nothing -> pure (Right cfg)
  Just (VTable t) -> case prepare t of
    Left err -> pure (Left err)
    Right session -> case M.lookup "keybinds" t of
      Nothing -> pure (Right session)
      Just (VTable binds) -> do
        overrides <- parseBindings (name ++ ".keybinds") cfg decode binds
        pure $ do
          changes <- overrides
          let common = M.fromList (keys cfg cfg)
              effective = foldl (\km (key, action) -> M.alter (const action) key km) common changes
          pure session {keys = const (M.toList effective)}
      Just v -> pure (Left (name ++ ".keybinds must be a table, not " ++ typeName v))
  Just v -> pure (Left (name ++ " must be a table, not " ++ typeName v))
  where
    prepare t = do
      session <- checkKeys (name ++ ".") ["autostart", "keybinds"] t cfg
      case M.lookup "autostart" t of
        Nothing -> pure session
        Just v -> do
          commands <- either (Left . ((name ++ ".autostart: ") ++)) Right (strList "autostart" v)
          pure session {startupHook = mapM_ spawn commands}
    decode (VBool False) = Right Nothing
    decode v = Just <$> (str v >>= parseAction cfg)

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

bool :: Value -> Result Bool
bool (VBool b) = Right b
bool v = Left ("expected true or false, got " ++ typeName v)

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

touchpadTable :: TouchpadSettings -> Value -> Result TouchpadSettings
touchpadTable tp (VTable t) = do
  _ <- checkKeys "touchpad." known t ()
  let get name def decode = maybe (Right def) (prefixed name . decode) (M.lookup name t)
      prefixed name = either (Left . ((name ++ ": ") ++)) Right
  tap <- get "tap" (tpTap tp) bool
  drag <- get "tap_and_drag" (tpTapDrag tp) bool
  dragLock <- get "drag_lock" (tpDragLock tp) bool
  natural <- get "natural_scroll" (tpNaturalScroll tp) bool
  dwt <- get "disable_while_typing" (tpDisableWhileTyping tp) bool
  speed <- get "speed" (tpAccelSpeed tp) accelSpeed
  profile <- get "accel_profile" (tpAccelProfile tp) $
    choice [("default", AccelDefault), ("adaptive", AccelAdaptive), ("flat", AccelFlat)]
  click <- get "click_method" (tpClickMethod tp) $
    choice [("default", ClickDefault), ("button-areas", ClickButtonAreas), ("clickfinger", ClickFinger)]
  Right (TouchpadSettings tap drag dragLock natural dwt speed profile click)
  where
    known =
      [ "tap", "tap_and_drag", "drag_lock", "natural_scroll", "disable_while_typing"
      , "speed", "accel_profile", "click_method"
      ]
    accelSpeed (VFloat d) | d >= -1 && d <= 1 = Right d
    accelSpeed (VInt n) | n >= -1 && n <= 1 = Right (fromIntegral n)
    accelSpeed v = Left ("expected a number from -1 to 1, got " ++ showValue v)
    choice opts v = str v >>= \s -> case lookup (map toLower s) opts of
      Just x -> Right x
      Nothing -> Left ("expected one of " ++ intercalate ", " (map fst opts) ++ ", not " ++ show s)
touchpadTable _ v = Left ("expected a table, got " ++ typeName v)

-- | [scratchpads."term"] with command, and optional app_id (default: the
-- name), width and height (fractions of the screen, default 0.6).
scratchpadTable :: Value -> Result [(String, Scratchpad)]
scratchpadTable (VTable ps) = mapM one (M.toList ps)
  where
    one (name, VTable p) = either (Left . ((name ++ ": ") ++)) Right $ do
      _ <- checkKeys "" ["command", "app_id", "width", "height"] p ()
      cmd <- maybe (Left "needs a command") str (M.lookup "command" p)
      app <- maybe (Right name) str (M.lookup "app_id" p)
      w <- maybe (Right 0.6) (fraction "width") (M.lookup "width" p)
      h <- maybe (Right 0.6) (fraction "height") (M.lookup "height" p)
      Right (name, Scratchpad cmd app w h)
    one (name, v) = Left (name ++ ": expected a table, got " ++ typeName v)
    fraction _ (VFloat d) | d >= 0.1 && d <= 1 = Right d
    fraction _ (VInt 1) = Right 1
    fraction what v = Left (what ++ " must be between 0.1 and 1, got " ++ showValue v)
scratchpadTable v = Left ("expected a table, got " ++ typeName v)

-- | [gestures] with "3-left" = "<action>": fingers (3-5) and a direction.
gestureTable :: Config -> Value -> Result [((Int, Int), X ())]
gestureTable cfg (VTable gs) = mapM one (M.toList gs)
  where
    one (k, v) = either (Left . (show k ++) . (": " ++)) Right $ do
      key <- case break (== '-') k of
        (n, '-' : dir) -> (,) <$> fingers n <*> direction (map toLower dir)
        _ -> Left "write it as <fingers>-<direction>, e.g. \"3-left\""
      act <- str v >>= parseAction cfg
      Right (key, act)
    fingers n = case readMaybe n of
      Just f | f >= 3 && f <= 5 -> Right f
      _ -> Left "swipes use 3 to 5 fingers (2 fingers scroll)"
    direction d = maybe (Left ("unknown direction " ++ show d ++ " (left, right, up, down)")) Right
      (lookup d (zip ["left", "right", "up", "down"] [0 ..]))
gestureTable _ v = Left ("expected a table, got " ++ typeName v)

-- | [rules."firefox"] with workspace = 2 and/or float = true|false.
ruleTable :: [Int] -> Value -> Result [(String, WindowRule)]
ruleTable tags (VTable rs) = mapM one (M.toList rs)
  where
    one (name, VTable r) = either (Left . ((name ++ ": ") ++)) Right $ do
      _ <- checkKeys "" ["workspace", "float"] r ()
      ws <- maybe (Right Nothing) (fmap Just . workspace) (M.lookup "workspace" r)
      fl <- maybe (Right Nothing) (fmap Just . bool) (M.lookup "float" r)
      Right (map toLower name, WindowRule ws fl)
    one (name, v) = Left (name ++ ": expected a table, got " ++ typeName v)
    workspace (VInt n) | fromIntegral n `elem` tags = Right (fromIntegral n)
    workspace v = Left ("no workspace " ++ showValue v)
ruleTable _ v = Left ("expected a table, got " ++ typeName v)

-- | [outputs."eDP-1"] with scale = 1.5 and/or position = [x, y].
outputTable :: Value -> Result [OutputRule]
outputTable (VTable outs) = mapM one (M.toList outs)
  where
    one (name, VTable o) = either (Left . ((name ++ ": ") ++)) Right $ do
      _ <- checkKeys "" ["scale", "position"] o ()
      sc <- maybe (Right 0) scale (M.lookup "scale" o)
      pos <- maybe (Right Nothing) (fmap Just . position) (M.lookup "position" o)
      Right (name, sc, pos)
    one (name, v) = Left (name ++ ": expected a table, got " ++ typeName v)
    scale (VFloat d) | d >= 0.5 && d <= 4 = Right d
    scale (VInt n) | n >= 1 && n <= 4 = Right (fromIntegral n)
    scale v = Left ("scale must be between 0.5 and 4, got " ++ showValue v)
    position (VArray [VInt x, VInt y]) = Right (fromIntegral x, fromIntegral y)
    position v = Left ("position is written [x, y], got " ++ typeName v)
outputTable v = Left ("expected a table, got " ++ typeName v)

showValue :: Value -> String
showValue (VInt n) = show n
showValue (VFloat d) = show d
showValue v = typeName v

-- Key bindings -----------------------------------------------------------

bindings :: Config -> Table -> IO (Result [((KeyMask, KeySym), X ())])
bindings cfg = parseBindings "keybinds" cfg (\v -> str v >>= parseAction cfg)

parseBindings :: String -> Config -> (Value -> Result a) -> Table -> IO (Result [((KeyMask, KeySym), a)])
parseBindings path cfg decode table = fmap sequence (mapM one (M.toList table))
  where
    one (combo, v) = do
      k <- parseCombo (modMask cfg) combo
      pure $ either (Left . ((path ++ "." ++ show combo ++ ": ") ++)) Right $ do
        key <- k
        act <- decode v
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
  [ "spawn <command>", "terminal", "close", "quit", "reload"
  , "focus-down", "focus-up", "swap-down", "swap-up", "swap-master"
  , "workspace <n>", "move-to-workspace <n>", "workspace-previous"
  , "layout <name>", "next-layout", "master-ratio <+/-delta>"
  , "toggle-float", "fullscreen"
  , "focus-screen next|prev", "move-to-screen next|prev"
  , "toggle-scratchpad <name>", "workspace-next", "workspace-prev"
  ]

parseAction :: Config -> String -> Result (X ())
parseAction cfg text = case words text of
  ("spawn" : _) -> case dropWhile isSpace (drop 5 (dropWhile isSpace text)) of
    "" -> Left "spawn needs a command"
    cmd -> Right (spawn cmd)
  ["terminal"] -> Right (spawn (terminal cfg))
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
  ["focus-screen", dir] -> focusScreenBy <$> direction dir
  ["move-to-screen", dir] -> moveToScreenBy <$> direction dir
  ["toggle-float"] -> Right toggleFloat
  ["fullscreen"] -> Right toggleFullscreen
  ["workspace-next"] -> Right (cycleWorkspace 1)
  ["workspace-prev"] -> Right (cycleWorkspace (-1))
  ["toggle-scratchpad", name]
    | name `elem` map fst (scratchpads cfg) -> Right (toggleScratchpad name)
    | otherwise -> Left ("no scratchpad " ++ show name ++ " in [scratchpads]")
  _ -> Left ("unknown action " ++ show text ++ " (known: " ++ intercalate ", " actionNames ++ ")")
  where
    direction d = case map toLower d of
      "next" -> Right 1
      "prev" -> Right (-1)
      "previous" -> Right (-1)
      _ -> Left ("expected next or prev, not " ++ show d)
    tag n = case readMaybe n of
      Just t | t `elem` workspaceTags cfg -> Right t
      _ -> Left ("no workspace " ++ show n)
