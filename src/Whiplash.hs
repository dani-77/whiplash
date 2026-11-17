-- | Everything a whiplash configuration needs, in one import.
module Whiplash
  ( module Whiplash.Core
  , module Whiplash.Keys
  , module Whiplash.Layout
  , focusUp
  , focusDown
  , swapUp
  , swapDown
  , swapMaster
  , view
  , shift
  , (.|.)
  ) where

import Data.Bits ((.|.))
import Whiplash.Core
import Whiplash.Keys
import Whiplash.Layout
import Whiplash.StackSet (focusDown, focusUp, shift, swapDown, swapMaster, swapUp, view)
