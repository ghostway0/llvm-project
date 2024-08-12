//===- WasmReader.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WasmReader.h"
#include "wasm/WasmObject.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Object/Wasm.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/LEB128.h"
#include <cassert>
#include <cstdint>
#include <optional>

namespace llvm {
namespace objcopy {
namespace wasm {

using namespace object;
using namespace llvm::wasm;

template <typename T> static uint64_t readULEB128(T Begin, T End) {
  unsigned Count;
  const char *Error = nullptr;
  uint64_t Result = decodeULEB128(Begin, &Count, End, &Error);
  if (Error)
    report_fatal_error(Error);
  return Result;
}

template <typename T> static uint32_t readVaruint32(T Begin, T End) {
  uint64_t Value = readULEB128(Begin, End);
  if (Value > UINT32_MAX) {
    report_fatal_error(make_error<GenericBinaryError>(
        "LEB is outside 32-bit bounds", object_error::parse_failed));
  }

  return Value;
}

// TODO:
// - [X] remove associated reloc section if a section is removed
// - [X] remove section symbols
// - [ ] remove associated symbols ?
// - [ ] rebuild and export linking section

Expected<std::unique_ptr<Object>> Reader::create() const {
  auto Obj = std::make_unique<Object>();
  Obj->Header = WasmObj.getHeader();

  Obj->Symbols.reserve(WasmObj.getNumberOfSymbols());
  for (const BasicSymbolRef SymbolRef : WasmObj.symbols()) {
    Obj->Symbols.push_back(WasmObj.getWasmSymbol(SymbolRef));
  }

  std::vector<Section> Sections;
  Obj->OpaqueSections.reserve(WasmObj.getNumSections());
  for (const SectionRef &Sec : WasmObj.sections()) {
    const WasmSection &WS = WasmObj.getWasmSection(Sec);

    if (WS.Type > WASM_SEC_LAST_KNOWN) {
      return make_error<GenericBinaryError>("Invalid section type",
                                            object_error::parse_failed);
    }

    if (WS.Type == WASM_SEC_CUSTOM && WS.Name.starts_with("reloc.")) {
      uint32_t ReferencedSectionIdx =
          readVaruint32(WS.Content.begin(), WS.Content.end());

      if (ReferencedSectionIdx >= Obj->OpaqueSections.size()) {
        return make_error<GenericBinaryError>(
            "Referenced section index in reloc section is outside bounds",
            object_error::parse_failed);
      }

      Obj->OpaqueSections[ReferencedSectionIdx].RelocationSectionIdx =
          Obj->OpaqueSections.size();
    }

    Obj->OpaqueSections.push_back({static_cast<uint8_t>(WS.Type),
                                   WS.HeaderSecSizeEncodingLen, WS.Name,
                                   WS.Content, std::nullopt});
    Section &ReaderSec = Obj->OpaqueSections.back();

    if (WS.Type == WASM_SEC_CUSTOM && WS.Name == "linking") {
      Obj->LinkingSection = &ReaderSec;
      Obj->LinkingData = WasmObj.linkingData();
    }

    if (ReaderSec.SectionType != WASM_SEC_CUSTOM) {
      // Give known sections standard names to allow them to be selected.
      // (Custom sections already have their names filled in by the parser).
      ReaderSec.Name = sectionTypeToString(ReaderSec.SectionType);
    }
  }

  Obj->DataSegments.reserve(WasmObj.dataSegments().size());
  llvm::copy(WasmObj.dataSegments(), Obj->DataSegments.begin());

  return std::move(Obj);
}

} // end namespace wasm
} // end namespace objcopy
} // end namespace llvm
