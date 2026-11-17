-- | Pure window bookkeeping, in the spirit of xmonad's StackSet: a set of
-- workspaces, each holding a zipper of windows with a focused element.
-- No IO here, so every operation can be reasoned about (and tested) alone.
module Whiplash.StackSet
  ( Stack (..)
  , Workspace (..)
  , StackSet (..)
  , new
  , integrate
  , currentWorkspace
  , peek
  , member
  , insertUp
  , delete
  , focusUp
  , focusDown
  , focusWindow
  , swapUp
  , swapDown
  , swapMaster
  , view
  , toggleWS
  , focusScreen
  , setScreens
  , visibleTags
  , shift
  , modifyLayout
  , allWindows
  ) where

import qualified Data.Map.Strict as M

-- | A list with a focused element: 'up' is stored reversed (nearest first).
data Stack a = Stack {up :: [a], focus :: a, down :: [a]}
  deriving (Show, Eq)

data Workspace l a = Workspace
  { tag :: Int
  , layoutIx :: l
  , stack :: Maybe (Stack a)
  }
  deriving (Show, Eq)

data StackSet l a = StackSet
  { current :: Int -- ^ the workspace on the focused screen
  , previous :: Int -- ^ last workspace viewed before 'current'
  , focusedScreen :: Int
  , screenTags :: M.Map Int Int -- ^ screen index -> workspace shown there
  , workspaces :: M.Map Int (Workspace l a)
  }
  deriving (Show, Eq)

new :: l -> [Int] -> StackSet l a
new l tags = StackSet first first 0 (M.singleton 0 first) (M.fromList [(t, Workspace t l Nothing) | t <- tags])
  where
    first = case tags of
      (t : _) -> t
      [] -> error "whiplash: workspaceTags must not be empty"

integrate :: Stack a -> [a]
integrate (Stack u f d) = reverse u ++ f : d

currentWorkspace :: StackSet l a -> Workspace l a
currentWorkspace s = workspaces s M.! current s

modifyCurrent :: (Workspace l a -> Workspace l a) -> StackSet l a -> StackSet l a
modifyCurrent f s = s {workspaces = M.adjust f (current s) (workspaces s)}

modifyStack :: (Stack a -> Maybe (Stack a)) -> StackSet l a -> StackSet l a
modifyStack f = modifyCurrent (\w -> w {stack = stack w >>= f})

peek :: StackSet l a -> Maybe a
peek = fmap focus . stack . currentWorkspace

allWindows :: StackSet l a -> [a]
allWindows = concatMap (maybe [] integrate . stack) . M.elems . workspaces

member :: Eq a => a -> StackSet l a -> Bool
member a = elem a . allWindows

-- | New windows go above the focused one and take focus, like xmonad.
insertUp :: Eq a => a -> StackSet l a -> StackSet l a
insertUp a s
  | member a s = s
  | otherwise = modifyCurrent (insertWs a) s

insertWs :: a -> Workspace l a -> Workspace l a
insertWs a w = w {stack = Just (maybe (Stack [] a []) (\(Stack u f d) -> Stack u a (f : d)) (stack w))}

-- | Remove a window from whichever workspace holds it. Focus moves down,
-- or up when the removed window was the last one.
delete :: Eq a => a -> StackSet l a -> StackSet l a
delete a s = s {workspaces = M.map del (workspaces s)}
  where
    del w = w {stack = stack w >>= remove}
    remove st@(Stack u f d)
      | f == a = case (u, d) of
          (_, d' : ds) -> Just (Stack u d' ds)
          (u' : us, []) -> Just (Stack us u' [])
          ([], []) -> Nothing
      | otherwise = Just st {up = filter (/= a) u, down = filter (/= a) d}

focusUp, focusDown, swapUp, swapDown, swapMaster :: StackSet l a -> StackSet l a
focusUp = modifyStack (Just . focusUp')
focusDown = modifyStack (Just . reverseStack . focusUp' . reverseStack)
swapUp = modifyStack (Just . swapUp')
swapDown = modifyStack (Just . reverseStack . swapUp' . reverseStack)
swapMaster = modifyStack (\(Stack u f d) -> Just (Stack [] f (reverse u ++ d)))

focusUp' :: Stack a -> Stack a
focusUp' (Stack (l : ls) f d) = Stack ls l (f : d)
focusUp' (Stack [] f d) = case reverse (f : d) of
  (x : xs) -> Stack xs x []
  [] -> Stack [] f d

swapUp' :: Stack a -> Stack a
swapUp' (Stack (l : ls) f d) = Stack ls f (l : d)
swapUp' (Stack [] f d) = Stack (reverse d) f []

reverseStack :: Stack a -> Stack a
reverseStack (Stack u f d) = Stack d f u

-- | Focus a specific window if it is on screen, moving to its screen.
focusWindow :: Eq a => a -> StackSet l a -> StackSet l a
focusWindow a s0 = case stack (currentWorkspace s) of
  Just st | a `elem` integrate st -> modifyStack (const (Just (refocus st))) s
  _ -> s
  where
    s = case [k | (k, t) <- M.toList (screenTags s0), a `elem` windowsOn t] of
      (k : _) -> focusScreen k s0
      [] -> s0
    windowsOn t = maybe [] (maybe [] integrate . stack) (M.lookup t (workspaces s0))
    refocus st =
      let xs = integrate st
          (before, rest) = break (== a) xs
       in case rest of
            (f : after) -> Stack (reverse before) f after
            [] -> st

-- | Show a workspace on the focused screen. If another screen shows it,
-- the two screens swap (xmonad's greedyView).
view :: Int -> StackSet l a -> StackSet l a
view t s
  | t == current s || not (M.member t (workspaces s)) = s
  | otherwise =
      let here = focusedScreen s
          tags = case [k | (k, t') <- M.toList (screenTags s), t' == t] of
            (k : _) -> M.insert k (current s) (screenTags s)
            [] -> screenTags s
       in s {current = t, previous = current s, screenTags = M.insert here t tags}

-- | Move focus to screen k (keeping what each screen shows).
focusScreen :: Int -> StackSet l a -> StackSet l a
focusScreen k s = case M.lookup k (screenTags s) of
  Just t -> s {focusedScreen = k, current = t}
  Nothing -> s

-- | Adapt to n screens: keep what surviving screens show, give new ones
-- the first workspaces not shown anywhere.
setScreens :: Int -> StackSet l a -> StackSet l a
setScreens n s
  | n < 1 = s
  | otherwise =
      let kept = M.filterWithKey (\k _ -> k < n) (screenTags s)
          free = [t | t <- M.keys (workspaces s), t `notElem` M.elems kept]
          missing = [k | k <- [0 .. n - 1], not (M.member k kept)]
          tags = M.union kept (M.fromList (zip missing free))
          fs = if focusedScreen s < n then focusedScreen s else 0
       in s {screenTags = tags, focusedScreen = fs, current = M.findWithDefault (current s) fs tags}

visibleTags :: StackSet l a -> [Int]
visibleTags = M.elems . screenTags

-- | Go back to the previously viewed workspace.
toggleWS :: StackSet l a -> StackSet l a
toggleWS s = view (previous s) s

-- | Move the focused window to another workspace (focus stays here).
shift :: Eq a => Int -> StackSet l a -> StackSet l a
shift t s = case peek s of
  Just a
    | t /= current s && M.member t (workspaces s) ->
        let s' = delete a s
         in s' {workspaces = M.adjust (insertWs a) t (workspaces s')}
  _ -> s

modifyLayout :: (l -> l) -> StackSet l a -> StackSet l a
modifyLayout f = modifyCurrent (\w -> w {layoutIx = f (layoutIx w)})
