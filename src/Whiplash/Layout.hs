{-# LANGUAGE RankNTypes #-}
-- | Layouts are pure functions from a screen rectangle and a list of windows
-- to a rectangle per window. The first window in the list is the master.
module Whiplash.Layout
  ( Rect (..)
  , Layout (..)
  , tall
  , wide
  , full
  , shrinkRect
  ) where

data Rect = Rect {rectX, rectY, rectW, rectH :: !Int}
  deriving (Show, Eq)

data Layout = Layout
  { layoutName :: String
  , -- | Master ratio (0..1) -> screen -> windows -> placements.
    arrange :: forall a. Double -> Rect -> [a] -> [(a, Rect)]
  }

-- | Master on the left, the rest stacked on the right.
tall :: Layout
tall = Layout "Tall" go
  where
    go _ _ [] = []
    go _ r [w] = [(w, r)]
    go ratio (Rect x y w h) (m : rest) =
      let mw = round (fromIntegral w * ratio)
       in (m, Rect x y mw h) : column (Rect (x + mw) y (w - mw) h) rest

-- | Master on top, the rest side by side underneath.
wide :: Layout
wide = Layout "Wide" go
  where
    go _ _ [] = []
    go _ r [w] = [(w, r)]
    go ratio (Rect x y w h) (m : rest) =
      let mh = round (fromIntegral h * ratio)
       in (m, Rect x y w mh) : row (Rect x (y + mh) w (h - mh)) rest

-- | Every window fills the screen; the focused one is raised on top.
full :: Layout
full = Layout "Full" (\_ r ws -> [(w, r) | w <- ws])

-- | Split a rectangle into equal horizontal bands, giving the remainder
-- pixels to the last band so nothing is lost to rounding.
column :: Rect -> [a] -> [(a, Rect)]
column (Rect x y w h) ws =
  [ (win, Rect x (y + i * band) w (if i == n - 1 then h - i * band else band))
  | (i, win) <- zip [0 ..] ws
  ]
  where
    n = length ws
    band = h `div` max 1 n

row :: Rect -> [a] -> [(a, Rect)]
row (Rect x y w h) ws =
  [ (win, Rect (x + i * band) y (if i == n - 1 then w - i * band else band) h)
  | (i, win) <- zip [0 ..] ws
  ]
  where
    n = length ws
    band = w `div` max 1 n

shrinkRect :: Int -> Rect -> Rect
shrinkRect g (Rect x y w h) = Rect (x + g) (y + g) (max 1 (w - 2 * g)) (max 1 (h - 2 * g))
