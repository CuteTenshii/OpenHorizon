#pragma once

#include "Microcode.h"

#include <string>

namespace openhorizon::xenos {

struct Shader;

/// A text listing of a program: each control flow instruction, and under
/// every exec the slots it runs as fetch or ALU instructions. The notation
/// follows the XNA/Xbox assembler loosely: `r#` temporaries, `c#` float
/// constants, `b#` boolean constants, `oPos`/`oPts`/`o#` vertex exports,
/// `oC#`/`oDepth` pixel exports, `_` for components left unwritten.
std::string disassemble(const Program& program);

/// One ALU slot, as in the listing.
std::string disassembleAlu(const AluInstruction& alu, ShaderStage stage);
/// One fetch slot, as in the listing.
std::string disassembleFetch(const FetchInstruction& fetch);
/// One control flow instruction, as in the listing.
std::string disassembleCf(const CfInstruction& cf);

/// Everything known about a shader: interface tables, constant table,
/// literals and the listing.
std::string describeShader(const Shader& shader);

} // namespace openhorizon::xenos
