//===- WasmObject.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WasmObject.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/Object/Wasm.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"
#include <MacTypes.h>
#include <_types/_uint8_t.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <stack>
#include <vector>

namespace llvm {
namespace objcopy {
namespace wasm {

using namespace object;
using namespace llvm::wasm;

void Object::addSectionWithOwnedContents(
    Section NewSection, std::unique_ptr<MemoryBuffer> &&Content) {
  OpaqueSections.push_back(NewSection);
  OwnedContents.emplace_back(std::move(Content));
}

void Object::removeSections(function_ref<bool(const Section &)> ToRemove) {
  std::vector<size_t> MarkedSections;
  MarkedSections.reserve(OpaqueSections.size());

  for (size_t I = 0; I < OpaqueSections.size(); I++) {
    const Section &S = OpaqueSections[I];
    if (ToRemove(S)) {
      MarkedSections.push_back(I);

      if (S.RelocationSectionIdx) {
        MarkedSections.push_back(S.RelocationSectionIdx.value());
      }
    }
  }

  std::sort(MarkedSections.begin(), MarkedSections.end());

  MarkedSections.erase(
      std::unique(MarkedSections.begin(), MarkedSections.end()),
      MarkedSections.end());

  if (MarkedSections.empty()) {
    return;
  }

  for (WasmSymbol &Sym : Symbols) {
    llvm::wasm::WasmSymbolInfo &Info = Sym.Info;

    auto Upper = std::upper_bound(MarkedSections.begin(), MarkedSections.end(),
                                  Info.ElementIndex);

    switch (Info.Kind) {
    case wasm::WASM_SYMBOL_TYPE_FUNCTION:
    case wasm::WASM_SYMBOL_TYPE_GLOBAL:
    case wasm::WASM_SYMBOL_TYPE_TAG:
    case wasm::WASM_SYMBOL_TYPE_TABLE:
      Info.ElementIndex -= std::distance(MarkedSections.end(), Upper);
      for (size_t I = *Upper; I < MarkedSections.size(); I++) {
        Section &S = OpaqueSections[I];

        size_t HeaderSize =
            S.HeaderSecSizeEncodingLen ? *S.HeaderSecSizeEncodingLen : 5;
        Info.DataRef.Offset -= S.Contents.size() - HeaderSize;
      }
      break;
    case wasm::WASM_SYMBOL_TYPE_DATA:
      if ((Info.Flags & wasm::WASM_SYMBOL_UNDEFINED) == 0) {
        for (size_t I = *Upper; I < MarkedSections.size(); I++) {
          Section &S = OpaqueSections[I];

          size_t HeaderSize =
              S.HeaderSecSizeEncodingLen ? *S.HeaderSecSizeEncodingLen : 5;
          Info.DataRef.Offset -= S.Contents.size() - HeaderSize;
        }
      }
      break;
    case wasm::WASM_SYMBOL_TYPE_SECTION: {
      Sym.Info.ElementIndex -= std::distance(MarkedSections.end(), Upper);
      break;
    }
    default:
      llvm_unreachable("unexpected kind");
    }
  }

  for (size_t I : reverse(MarkedSections)) {
    OpaqueSections.erase(OpaqueSections.begin() + I);
  }

  std::vector<size_t> MarkedSymbols;

  for (size_t I = 0; I < Symbols.size(); I++) {
    const WasmSymbol &WS = Symbols[I];
    if (WS.isTypeSection() &&
        std::binary_search(MarkedSections.begin(), MarkedSections.end(),
                           WS.Info.ElementIndex)) {
      MarkedSymbols.push_back(I);
    }
  }

  for (size_t I : reverse(MarkedSymbols)) {
    Symbols.erase(Symbols.begin() + I);
  }

  for (WasmSymbol &Sym : Symbols) {
    llvm::wasm::WasmSymbolInfo &Info = Sym.Info;
    printf("not removed: %s\n", Info.Name.data());
  }
}

// this whole thing is one big hack
class SectionWriter {
public:
  SectionWriter() = default;

  void writeULEB128(uint64_t Value, size_t PadTo = 0) {
    Buffer.resize(Cursor + 10);
    Cursor += encodeULEB128(Value, Buffer.data() + Cursor, PadTo);
  }

  void insertULEB128(uint64_t Value, uint64_t Offset) {
    encodeULEB128(Value, Buffer.data() + Offset);
  }

  void writeSLEB128(uint64_t Value) {
    Buffer.resize(Cursor + 10);
    Cursor += encodeSLEB128(Value, Buffer.data() + Cursor);
  }

  void writeVaruint32(uint32_t Value) {
    assert(Value <= 0x7fffffff);
    writeULEB128(Value);
  }

  size_t cursor() { return Cursor; }

  void writeBytes(iterator_range<const unsigned char *> Bytes) {
    Buffer.insert(Buffer.begin() + Cursor, Bytes.begin(), Bytes.end());
    Cursor += Bytes.end() - Bytes.begin();
  }

  void writeString(StringRef Str) { writeBytes(Str.bytes()); }

  void startSubsection(uint8_t Kind) {
    Buffer.push_back(Kind);
    Cursor += 1;
    Subsections.push(Cursor);
    writeULEB128(0, 5); // placeholder
  }

  size_t endSubsection() {
    size_t Offset = Subsections.top();
    insertULEB128(Cursor - Offset, Offset);
    Subsections.pop();

    return Cursor - Offset;
  }

  std::vector<uint8_t> finalize() {
    assert(Subsections.empty() && "Unclosed subsections are still pending.");
    Buffer.resize(Cursor);
    return Buffer;
  }

private:
  std::vector<uint8_t> Buffer;
  std::stack<size_t> Subsections;
  size_t Cursor = 0;
};

// this is pretty much a hack. you could try having some
// owned/non-owned capable structures instead of ArrayRef.
std::vector<uint8_t> Object::finalizeLinking() {
  SectionWriter Writer;
  Writer.writeVaruint32(llvm::wasm::WasmMetadataVersion);

  if (!Symbols.empty()) {
    Writer.startSubsection(wasm::WASM_SYMBOL_TABLE);

    Writer.writeULEB128(Symbols.size());
    size_t previous = 0;
    for (const WasmSymbol &Sym : Symbols) {
      printf("%lu\n", Writer.cursor() - previous);
      previous = Writer.cursor();
      printf("Symbol: %s\n", Sym.Info.Name.data());
      printf("Symbol: %u\n", Sym.Info.Kind);
      llvm::wasm::WasmSymbolInfo Info = Sym.Info;
      Writer.writeULEB128(Info.Kind);
      Writer.writeULEB128(Info.Flags);

      switch (Info.Kind) {
      case wasm::WASM_SYMBOL_TYPE_FUNCTION:
      case wasm::WASM_SYMBOL_TYPE_GLOBAL:
      case wasm::WASM_SYMBOL_TYPE_TAG:
      case wasm::WASM_SYMBOL_TYPE_TABLE:
        Writer.writeULEB128(Info.ElementIndex);
        if ((Info.Flags & wasm::WASM_SYMBOL_UNDEFINED) == 0 ||
            (Info.Flags & wasm::WASM_SYMBOL_EXPLICIT_NAME) != 0)
          Writer.writeString(Info.Name);
        break;
      case wasm::WASM_SYMBOL_TYPE_DATA:
        Writer.writeString(Info.Name);
        if ((Info.Flags & wasm::WASM_SYMBOL_UNDEFINED) == 0) {
          Writer.writeULEB128(Info.DataRef.Segment);
          Writer.writeULEB128(Info.DataRef.Offset); // this has to be patched
          Writer.writeULEB128(Info.DataRef.Size);
        }
        break;
      case wasm::WASM_SYMBOL_TYPE_SECTION: {
        // Not Sure
        Writer.writeULEB128(Sym.Info.ElementIndex);
        break;
      }
      default:
        llvm_unreachable("unexpected kind");
      }
    }
    printf("%lu\n", Writer.endSubsection());
  }

  if (!DataSegments.empty()) {
    printf("Write DataSegments %lu\n", DataSegments.size());
    Writer.startSubsection(wasm::WASM_SEGMENT_INFO);
    Writer.writeVaruint32(DataSegments.size());
    for (const object::WasmSegment &Segment : DataSegments) {
      Writer.writeString(Segment.Data.Name);
      Writer.writeULEB128(Segment.Data.Alignment);
      Writer.writeULEB128(Segment.Data.LinkingFlags);
    }
    printf("%lu\n", Writer.endSubsection());
  }

  if (!LinkingData.InitFunctions.empty()) {
    printf("Write InitFunctions %lu\n", LinkingData.InitFunctions.size());
    Writer.startSubsection(wasm::WASM_INIT_FUNCS);
    Writer.writeULEB128(LinkingData.InitFunctions.size());
    for (WasmInitFunc &InitFunc : LinkingData.InitFunctions) {
      Writer.writeULEB128(InitFunc.Priority);
      Writer.writeULEB128(InitFunc.Symbol);
    }
    printf("%lu\n", Writer.endSubsection());
  }

  if (!LinkingData.Comdats.empty()) {
    printf("Write Comdats %lu\n", LinkingData.Comdats.size());
    Writer.startSubsection(wasm::WASM_COMDAT_INFO);
    Writer.writeULEB128(LinkingData.Comdats.size());
    for (const StringRef &C : LinkingData.Comdats) {
      Writer.writeString(C);
      Writer.writeULEB128(0); // flags for future use
      Writer.writeULEB128(C.size());
      // FIXME
    }
    printf("%lu\n", Writer.endSubsection());
  }

  return Writer.finalize();
}

} // end namespace wasm
} // end namespace objcopy
} // end namespace llvm
