{-# LANGUAGE BangPatterns #-}

module Main where

import qualified Cardano.Crypto.Wallet as CC
import qualified Cardano.Crypto.Wallet.Encrypted as CC
import           Data.ByteArray
import           Data.ByteString (ByteString)
import qualified Data.ByteString as S
import           Weigh

main =
  mainWith
    (sequence
       [ func
         ("CC.generate: " ++ show n)
         (flip CC.generate (mempty :: ScrubbedBytes))
         s
       | (n, !s) <-
           map (\n -> (n, S.replicate n 0)) [32, 32*10, 32*100, 32*1000, 32*10000]
       ])
