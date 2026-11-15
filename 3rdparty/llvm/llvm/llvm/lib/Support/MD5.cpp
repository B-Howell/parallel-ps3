//===-- MD5.cpp - MD5 implementation -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// llvm::MD5 on libretro-common's MD5 (utils/md5.c, lrc_hash.h): LLVM's own
// copy of the algorithm is gone. MD5Result stays LLVM's type, since the
// checksums of the MC layer and DWARF are kept in it.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MD5.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include <lrc_hash.h> // libretro-common's MD5, in place of LLVM's own
#include <cstring>

using namespace llvm;

static_assert(sizeof(MD5_CTX) == 152, "MD5::InternalState holds an MD5_CTX");

static MD5_CTX *ctx(unsigned char *State) {
  return reinterpret_cast<MD5_CTX *>(State);
}

MD5::MD5() { MD5_Init(ctx(InternalState)); }

/// Incrementally add the bytes in \p Data to the hash.
void MD5::update(ArrayRef<uint8_t> Data) {
  const uint8_t *Ptr = Data.data();
  size_t Size = Data.size();

  // MD5_Update counts in an unsigned long, 32 bits on Windows
  while (Size) {
    const unsigned long Part =
        Size > 0x40000000u ? 0x40000000ul : static_cast<unsigned long>(Size);
    MD5_Update(ctx(InternalState), Ptr, Part);
    Ptr += Part;
    Size -= Part;
  }
}

/// Add the bytes in the StringRef \p Str to the hash.
// Note that this isn't a string and so this won't include any trailing NULL
// bytes.
void MD5::update(StringRef Str) {
  update(ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Str.data()),
                           Str.size()));
}

/// Finish the hash and place the resulting hash into \p result.
void MD5::final(MD5Result &Result) {
  MD5_Final(Result.data(), ctx(InternalState));
}

MD5::MD5Result MD5::final() {
  MD5Result Result;
  final(Result);
  return Result;
}

MD5::MD5Result MD5::result() {
  MD5_CTX Copy;
  std::memcpy(&Copy, InternalState, sizeof(Copy));

  MD5Result Hash;
  MD5_Final(Hash.data(), &Copy);
  return Hash;
}

SmallString<32> MD5::MD5Result::digest() const {
  SmallString<32> Str;
  toHex(*this, /*LowerCase*/ true, Str);
  return Str;
}

void MD5::stringifyResult(MD5Result &Result, SmallVectorImpl<char> &Str) {
  toHex(Result, /*LowerCase*/ true, Str);
}

MD5::MD5Result MD5::hash(ArrayRef<uint8_t> Data) {
  MD5 Hash;
  Hash.update(Data);
  MD5::MD5Result Res;
  Hash.final(Res);

  return Res;
}
