//
//  BWBIP39Mnemonic.h
//
//  Created by Aaron Voisine on 9/7/15.
//  Copyright (c) 2015 breadwallet LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#ifndef BWBIP39Mnemonic_h
#define BWBIP39Mnemonic_h

#include <stddef.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// BIP39 is method for generating a deterministic wallet seed from a mnemonic phrase
// https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki

//  Starting TIME   Sync block      Notes
//  1618643881      2036160         Sync starts in April 2018
//  1619999000               Sync starts in April 2021

#define BIP39_CREATION_TIME 1464739200 // oldest possible BIP39 phrase creation time, Wednesday, June 1, 2016 12:00:00 AM
#define MID_BLOCK_TIME 1524838967 //  4/27/2018 2:22:47 PM - Blockheight: 1411200
#define RECENT_BLOCK_TIME 1618643881 // 4/17/2021 7:18:01 AM - Blockheight: 2036160
#define BIP39_WORDLIST_COUNT 2048       // number of words in a BIP39 wordlist

// returns number of bytes written to phrase including NULL terminator, or phraseLen needed if phrase is NULL
size_t BWBIP39Encode(char *phrase, size_t phraseLen, const char *wordList[], const uint8_t *data, size_t dataLen);

// returns number of bytes written to data, or dataLen needed if data is NULL
size_t BWBIP39Decode(uint8_t *data, size_t dataLen, const char *wordList[], const char *phrase);

// verifies that all phrase words are contained in wordlist and checksum is valid
int BWBIP39PhraseIsValid(const char *wordList[], const char *phrase);

// key64 must hold 64 bytes (512 bits), phrase and passphrase must be unicode NFKD normalized
// http://www.unicode.org/reports/tr15/#Norm_Forms
// BUG: does not currently support passphrases containing NULL characters
void BWBIP39DeriveKey(void *key64, const char *phrase, const char *passphrase);

#ifdef __cplusplus
}
#endif

#endif // BWBIP39Mnemonic_h