-- | Everything a whiplash configuration needs, in one import.
module Whiplash
  ( module Whiplash.Core
  , module Whiplash.Run
  , module Whiplash.Keys
  , module Whiplash.Layout
  , focusUp
  , focusDown
  , swapUp
  , swapDown
  , swapMaster
  , view
  , toggleWS
  , shift
  , (.|.)
  ) where

import Data.Bits ((.|.))
import Whiplash.Core
import Whiplash.Run
import Whiplash.Keys
import Whiplash.Layout
import Whiplash.StackSet (focusDown, focusUp, shift, swapDown, swapMaster, swapUp, toggleWS, view)
