#ifndef LLVM_TARGETPARSER_MODLOADERADDRESSSPACES_H
#define LLVM_TARGETPARSER_MODLOADERADDRESSSPACES_H

#include "llvm/ADT/StringRef.h"
#include <iterator>
#include <optional>
#include <string>

namespace llvm::ModLoader {

enum class Codec { Flat, LittleEndian, BigEndian, WordSwapped };

inline constexpr StringLiteral CodecNames[] = {"flat", "little_endian",
                                               "big_endian", "word_swapped"};

inline std::optional<Codec> parseCodec(StringRef Name) {
  for (unsigned Index = 0; Index != std::size(CodecNames); ++Index)
    if (Name == CodecNames[Index])
      return static_cast<Codec>(Index);

  return std::nullopt;
}

inline StringRef getCodecName(Codec Value) {
  unsigned Index = static_cast<unsigned>(Value);
  return Index < std::size(CodecNames) ? StringRef(CodecNames[Index])
                                       : StringRef();
}

constexpr unsigned FirstGuestAddressSpace = 256;
constexpr unsigned MaxAddressSpace = (1u << 24) - 1;

constexpr bool isGuestAddressSpace(unsigned AS) {
  return AS >= FirstGuestAddressSpace && AS <= MaxAddressSpace;
}

constexpr unsigned getGuestAddressSpace(unsigned Id, unsigned Width) {
  return FirstGuestAddressSpace + 2 * Id + (Width == 64);
}

constexpr unsigned getGuestSpaceId(unsigned AS) {
  return (AS - FirstGuestAddressSpace) / 2;
}

constexpr unsigned getGuestPointerWidth(unsigned AS) {
  return (AS & 1) ? 64 : 32;
}

inline std::string withoutGuestPointerLayouts(StringRef Layout) {
  std::string Result;
  while (!Layout.empty()) {
    auto [Component, Rest] = Layout.split('-');
    Layout = Rest;
    StringRef PointerLayout = Component;
    if (PointerLayout.consume_front("p")) {
      auto [Number, Widths] = PointerLayout.split(':');
      unsigned AS;
      if (!Number.getAsInteger(10, AS) && isGuestAddressSpace(AS) &&
          Widths == (getGuestPointerWidth(AS) == 32 ? "32:32" : "64:64"))
        continue;
    }

    if (!Result.empty())
      Result += '-';
    Result += Component;
  }

  return Result;
}

} // namespace llvm::ModLoader

#endif
