//===- DXILPrettyPrinter.cpp - Print resources for textual DXIL -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DXILPrettyPrinter.h"
#include "DirectX.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/DXILResource.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static StringRef getRCName(dxil::ResourceClass RC) {
  switch (RC) {
  case dxil::ResourceClass::SRV:
    return "texture";
  case dxil::ResourceClass::UAV:
    return "UAV";
  case dxil::ResourceClass::CBuffer:
    return "cbuffer";
  case dxil::ResourceClass::Sampler:
    return "sampler";
  }
  llvm_unreachable("covered switch");
}

static StringRef getRCPrefix(dxil::ResourceClass RC) {
  switch (RC) {
  case dxil::ResourceClass::SRV:
    return "t";
  case dxil::ResourceClass::UAV:
    return "u";
  case dxil::ResourceClass::CBuffer:
    return "cb";
  case dxil::ResourceClass::Sampler:
    return "s";
  }
  llvm_unreachable("covered switch");
}

static StringRef getFormatName(const dxil::ResourceTypeInfo &RI) {
  if (RI.isTyped()) {
    switch (RI.getTyped().ElementTy) {
    case dxil::ElementType::I1:
      return "i1";
    case dxil::ElementType::I16:
      return "i16";
    case dxil::ElementType::U16:
      return "u16";
    case dxil::ElementType::I32:
      return "i32";
    case dxil::ElementType::U32:
      return "u32";
    case dxil::ElementType::I64:
      return "i64";
    case dxil::ElementType::U64:
      return "u64";
    case dxil::ElementType::F16:
      return "f16";
    case dxil::ElementType::F32:
      return "f32";
    case dxil::ElementType::F64:
      return "f64";
    case dxil::ElementType::SNormF16:
      return "snorm_f16";
    case dxil::ElementType::UNormF16:
      return "unorm_f16";
    case dxil::ElementType::SNormF32:
      return "snorm_f32";
    case dxil::ElementType::UNormF32:
      return "unorm_f32";
    case dxil::ElementType::SNormF64:
      return "snorm_f64";
    case dxil::ElementType::UNormF64:
      return "unorm_f64";
    case dxil::ElementType::PackedS8x32:
      return "p32i8";
    case dxil::ElementType::PackedU8x32:
      return "p32u8";
    case dxil::ElementType::Invalid:
      llvm_unreachable("Invalid ElementType");
    }
    llvm_unreachable("Unhandled ElementType");
  } else if (RI.isStruct())
    return "struct";
  else if (RI.isCBuffer() || RI.isSampler())
    return "NA";
  return "byte";
}

static StringRef getTextureDimName(dxil::ResourceKind RK) {
  switch (RK) {
  case dxil::ResourceKind::Texture1D:
    return "1d";
  case dxil::ResourceKind::Texture2D:
    return "2d";
  case dxil::ResourceKind::Texture3D:
    return "3d";
  case dxil::ResourceKind::TextureCube:
    return "cube";
  case dxil::ResourceKind::Texture1DArray:
    return "1darray";
  case dxil::ResourceKind::Texture2DArray:
    return "2darray";
  case dxil::ResourceKind::TextureCubeArray:
    return "cubearray";
  case dxil::ResourceKind::TBuffer:
    return "tbuffer";
  case dxil::ResourceKind::FeedbackTexture2D:
    return "fbtex2d";
  case dxil::ResourceKind::FeedbackTexture2DArray:
    return "fbtex2darray";
  case dxil::ResourceKind::Texture2DMS:
    return "2dMS";
  case dxil::ResourceKind::Texture2DMSArray:
    return "2darrayMS";
  case dxil::ResourceKind::Invalid:
  case dxil::ResourceKind::NumEntries:
  case dxil::ResourceKind::CBuffer:
  case dxil::ResourceKind::RawBuffer:
  case dxil::ResourceKind::Sampler:
  case dxil::ResourceKind::StructuredBuffer:
  case dxil::ResourceKind::TypedBuffer:
  case dxil::ResourceKind::RTAccelerationStructure:
    llvm_unreachable("Invalid ResourceKind for texture");
  }
  llvm_unreachable("Unhandled ResourceKind");
}

namespace {
struct FormatResourceDimension
    : public llvm::FormatAdapter<const dxil::ResourceTypeInfo &> {
  FormatResourceDimension(const dxil::ResourceTypeInfo &RI, bool HasCounter)
      : llvm::FormatAdapter<const dxil::ResourceTypeInfo &>(RI),
        HasCounter(HasCounter) {}

  bool HasCounter;

  void format(llvm::raw_ostream &OS, StringRef Style) override {
    dxil::ResourceKind RK = Item.getResourceKind();
    switch (RK) {
    default: {
      OS << getTextureDimName(RK);
      if (Item.isMultiSample())
        OS << Item.getMultiSampleCount();
      break;
    }
    case dxil::ResourceKind::RawBuffer:
    case dxil::ResourceKind::StructuredBuffer:
      if (!Item.isUAV())
        OS << "r/o";
      else if (HasCounter)
        OS << "r/w+cnt";
      else
        OS << "r/w";
      break;
    case dxil::ResourceKind::TypedBuffer:
      OS << "buf";
      break;
    case dxil::ResourceKind::CBuffer:
      OS << "NA";
      break;
    case dxil::ResourceKind::RTAccelerationStructure:
      // TODO: dxc would print "ras" here. Can/should this happen?
      llvm_unreachable("RTAccelerationStructure printing is not implemented");
    }
  }
};

struct FormatBindingID
    : public llvm::FormatAdapter<const dxil::ResourceInfo &> {
  dxil::ResourceClass RC;

  explicit FormatBindingID(const dxil::ResourceInfo &RI,
                           const dxil::ResourceTypeInfo &RTI)
      : llvm::FormatAdapter<const dxil::ResourceInfo &>(RI),
        RC(RTI.getResourceClass()) {}

  void format(llvm::raw_ostream &OS, StringRef Style) override {
    OS << getRCPrefix(RC).upper() << Item.getBinding().RecordID;
  }
};

struct FormatBindingLocation
    : public llvm::FormatAdapter<const dxil::ResourceInfo &> {
  dxil::ResourceClass RC;

  explicit FormatBindingLocation(const dxil::ResourceInfo &RI,
                                 const dxil::ResourceTypeInfo &RTI)
      : llvm::FormatAdapter<const dxil::ResourceInfo &>(RI),
        RC(RTI.getResourceClass()) {}

  void format(llvm::raw_ostream &OS, StringRef Style) override {
    const auto &Binding = Item.getBinding();
    OS << getRCPrefix(RC) << Binding.LowerBound;
    if (Binding.Space)
      OS << ",space" << Binding.Space;
  }
};

struct FormatBindingSize
    : public llvm::FormatAdapter<const dxil::ResourceInfo &> {
  explicit FormatBindingSize(const dxil::ResourceInfo &RI)
      : llvm::FormatAdapter<const dxil::ResourceInfo &>(RI) {}

  void format(llvm::raw_ostream &OS, StringRef Style) override {
    uint32_t Size = Item.getBinding().Size;
    if (Size == std::numeric_limits<uint32_t>::max())
      OS << "unbounded";
    else
      OS << Size;
  }
};

} // namespace

static void prettyPrintResources(raw_ostream &OS, const DXILResourceMap &DRM,
                                 DXILResourceTypeMap &DRTM) {
  // Column widths are arbitrary but match the widths DXC uses.
  OS << ";\n; Resource Bindings:\n;\n";
  OS << formatv("; {0,-30} {1,10} {2,7} {3,11} {4,7} {5,14} {6,9}\n", "Name",
                "Type", "Format", "Dim", "ID", "HLSL Bind", "Count");
  OS << formatv(
      "; {0,-+30} {1,-+10} {2,-+7} {3,-+11} {4,-+7} {5,-+14} {6,-+9}\n", "", "",
      "", "", "", "", "");

  // TODO: Do we want to sort these by binding or something like that?
  for (const dxil::ResourceInfo &RI : DRM) {
    const dxil::ResourceTypeInfo &RTI = DRTM[RI.getHandleTy()];

    dxil::ResourceClass RC = RTI.getResourceClass();
    StringRef Name(RI.getName());
    StringRef Type(getRCName(RC));
    StringRef Format(getFormatName(RTI));
    FormatResourceDimension Dim(RTI, RI.hasCounter());
    FormatBindingID ID(RI, RTI);
    FormatBindingLocation Bind(RI, RTI);
    FormatBindingSize Count(RI);
    OS << formatv("; {0,-30} {1,10} {2,7} {3,11} {4,7} {5,14} {6,9}\n", Name,
                  Type, Format, Dim, ID, Bind, Count);
  }
  OS << ";\n";
}

PreservedAnalyses DXILPrettyPrinterPass::run(Module &M,
                                             ModuleAnalysisManager &MAM) {
  const DXILResourceMap &DRM = MAM.getResult<DXILResourceAnalysis>(M);
  DXILResourceTypeMap &DRTM = MAM.getResult<DXILResourceTypeAnalysis>(M);
  prettyPrintResources(OS, DRM, DRTM);
  return PreservedAnalyses::all();
}

namespace {
class DXILPrettyPrinterLegacy : public llvm::ModulePass {
  raw_ostream &OS; // raw_ostream to print to.

public:
  static char ID;

  explicit DXILPrettyPrinterLegacy(raw_ostream &O) : ModulePass(ID), OS(O) {}

  StringRef getPassName() const override {
    return "DXIL Metadata Pretty Printer";
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<DXILResourceTypeWrapperPass>();
    AU.addRequired<DXILResourceWrapperPass>();
  }
};
} // namespace

char DXILPrettyPrinterLegacy::ID = 0;
INITIALIZE_PASS_BEGIN(DXILPrettyPrinterLegacy, "dxil-pretty-printer",
                      "DXIL Metadata Pretty Printer", true, true)
INITIALIZE_PASS_DEPENDENCY(DXILResourceTypeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DXILResourceWrapperPass)
INITIALIZE_PASS_END(DXILPrettyPrinterLegacy, "dxil-pretty-printer",
                    "DXIL Metadata Pretty Printer", true, true)

bool DXILPrettyPrinterLegacy::runOnModule(Module &M) {
  const DXILResourceMap &DRM =
      getAnalysis<DXILResourceWrapperPass>().getBindingMap();
  DXILResourceTypeMap &DRTM =
      getAnalysis<DXILResourceTypeWrapperPass>().getResourceTypeMap();
  prettyPrintResources(OS, DRM, DRTM);
  return false;
}

ModulePass *llvm::createDXILPrettyPrinterLegacyPass(raw_ostream &OS) {
  return new DXILPrettyPrinterLegacy(OS);
}
