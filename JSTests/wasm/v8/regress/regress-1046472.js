// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --liftoff --no-wasm-tier-up --wasm-staging

load('wasm-module-builder.js');

(function() {
  const builder = new WasmModuleBuilder();
  builder.addMemory(16, 32, false);
  builder.addType(makeSig([kWasmI32, kWasmI32, kWasmI32], [kWasmI32]));
  // Generate function 1 (out of 1).
  builder.addFunction('main', 0 /* sig */)
    .addBodyWithEnd([
// signature: i_iii
// body:
kExprI32Const, 0x20,
kExprI64LoadMem, 0x00, 0xce, 0xf2, 0xff, 0x01,
kExprBlock, kWasmF32,   // @9 f32
  kExprI32Const, 0x04,
  kExprI32Const, 0x01,
  kExprBrTable, 0x01, 0x01, 0x00, // entries=1
  kExprEnd,   // @19
kExprUnreachable,
kExprEnd,   // @21
            ]);
  builder.addExport('main', 0);
  assertThrows(
      () => {builder.toModule()}, WebAssembly.CompileError,
      `WebAssembly.Module doesn't validate: br_table target type mismatch at offset 0 expected: F32 but saw: I32 when targeting block: (I32, I32, I32) -> [I32], in function at index 0 (evaluating 'new WebAssembly.Module(this.toBuffer(debug))')`);
})();
