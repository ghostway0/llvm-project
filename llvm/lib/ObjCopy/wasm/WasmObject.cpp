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

  // patch symbols
  if (!Symbols.empty()) {
    for (WasmSymbol &Sym : Symbols) {
      llvm::wasm::WasmSymbolInfo &Info = Sym.Info;

      auto Upper = std::upper_bound(MarkedSections.begin(),
                                    MarkedSections.end(), Info.ElementIndex);

      switch (Info.Kind) {
      case wasm::WASM_SYMBOL_TYPE_FUNCTION:
      case wasm::WASM_SYMBOL_TYPE_GLOBAL:
      case wasm::WASM_SYMBOL_TYPE_TAG:
      case wasm::WASM_SYMBOL_TYPE_TABLE:
        Info.ElementIndex -= std::distance(MarkedSections.end(), Upper);
        for (size_t I = *Upper; I < MarkedSections.size(); I++) {
          Section &S = OpaqueSections[I];

          size_t HeaderSize = S.HeaderSecSizeEncodingLen ? *S.HeaderSecSizeEncodingLen : 5;
          Info.DataRef.Offset -= S.Contents.size() - HeaderSize;
        }
        break;
      case wasm::WASM_SYMBOL_TYPE_DATA:
        if ((Info.Flags & wasm::WASM_SYMBOL_UNDEFINED) == 0) {
          for (size_t I = *Upper; I < MarkedSections.size(); I++) {
            Section &S = OpaqueSections[I];

            size_t HeaderSize = S.HeaderSecSizeEncodingLen ? *S.HeaderSecSizeEncodingLen : 5;
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
  }

  size_t Removed = 0;
  for (size_t I : reverse(MarkedSections)) {
    OpaqueSections.erase(OpaqueSections.begin() + (I - Removed));
    Removed++;
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

  Removed = 0;
  for (size_t I : MarkedSymbols) {
    Symbols.erase(Symbols.begin() + (I - Removed));
    Removed++;
  }
}

// this whole thing is one big hack
class SectionWriter {
public:
  SectionWriter() = default;

  void writeULEB128(uint64_t Value) {
    Buffer.reserve(Buffer.size() + 10);
    encodeULEB128(Value, Buffer.data() + Buffer.size());
  }

  void writeSLEB128(uint64_t Value) {
    Buffer.reserve(Buffer.size() + 10);
    encodeSLEB128(Value, Buffer.data() + Buffer.size());
  }

  void writeVaruint32(uint32_t Value) { writeULEB128(Value); }

  void writeBytes(iterator_range<const unsigned char *> Bytes) {
    Buffer.insert(Buffer.end(), Bytes.begin(), Bytes.end());
  }

  void writeString(StringRef Str) { writeBytes(Str.bytes()); }

  void startSubsection() { Subsections.push(Buffer.size()); }

  void startSubsection(uint8_t Kind) { Subsections.push(Buffer.size()); }

  size_t endSubsection() {
    size_t T = Subsections.top();
    Subsections.pop();

    return T;
  }

  std::vector<uint8_t> finalize() {
    assert(Subsections.empty() && "Unclosed subsections are still pending.");
    return Buffer;
  }

private:
  std::vector<uint8_t> Buffer;
  std::stack<size_t> Subsections;
};

// this is pretty much a hack. you could try having some
// owned/non-owned capable structures instead of ArrayRef.
std::vector<uint8_t> Object::finalizeLinking() {
  SectionWriter Writer;
  Writer.writeULEB128(llvm::wasm::WasmMetadataVersion);

  if (!Symbols.empty()) {
    Writer.startSubsection(wasm::WASM_SYMBOL_TABLE);
    Writer.writeULEB128(Symbols.size());
    for (const WasmSymbol &Sym : Symbols) {
      llvm::wasm::WasmSymbolInfo Info = Sym.Info;

      Writer.writeULEB128(Info.Kind);
      Writer.writeULEB128(Info.Flags);
      switch (Info.Kind) {
      case wasm::WASM_SYMBOL_TYPE_FUNCTION:
      case wasm::WASM_SYMBOL_TYPE_GLOBAL:
      case wasm::WASM_SYMBOL_TYPE_TAG:
      case wasm::WASM_SYMBOL_TYPE_TABLE:
        // this has to be patched
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
        // FIXME: implement this
        break;
      }
      default:
        llvm_unreachable("unexpected kind");
      }
    }
    Writer.endSubsection();
  }

  if (!DataSegments.empty()) {
    Writer.startSubsection(wasm::WASM_SEGMENT_INFO);
    Writer.writeULEB128(DataSegments.size());
    for (const object::WasmSegment &Segment : DataSegments) {
      Writer.writeString(Segment.Data.Name);
      Writer.writeULEB128(Segment.Data.Alignment);
      Writer.writeULEB128(Segment.Data.LinkingFlags);
    }
    Writer.endSubsection();
  }

  if (!LinkingData.InitFunctions.empty()) {
    Writer.startSubsection(wasm::WASM_INIT_FUNCS);
    Writer.writeULEB128(LinkingData.InitFunctions.size());
    for (WasmInitFunc &InitFunc : LinkingData.InitFunctions) {
      Writer.writeULEB128(InitFunc.Priority);
      Writer.writeULEB128(InitFunc.Symbol);
    }
    Writer.endSubsection();
  }

  if (!LinkingData.Comdats.empty()) {
    Writer.startSubsection(wasm::WASM_COMDAT_INFO);
    Writer.writeULEB128(LinkingData.Comdats.size());
    for (const StringRef &C : LinkingData.Comdats) {
      Writer.writeString(C);
      Writer.writeULEB128(0); // flags for future use
      Writer.writeULEB128(C.size());
      // FIXME
    }
    Writer.endSubsection();
  }

  return Writer.finalize();
}

} // end namespace wasm
} // end namespace objcopy
} // end namespace llvm
