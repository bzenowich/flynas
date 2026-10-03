/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// Check the DragonFly aarch64 target in clangBasic/clangDriver: the
// predefined macros and the jobs the driver builds (like clang -###).
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <memory>
#include <vector>

using namespace clang;

int main(int argc, const char **argv)
{
	IntrusiveRefCntPtr<DiagnosticOptions> dopts = new DiagnosticOptions();
	IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());
	DiagnosticsEngine diags(ids, &*dopts);
	diags.setClient(new IgnoringDiagConsumer());

	const char *triple = argc > 1 ? argv[1] : "aarch64-unknown-dragonfly";
	if (argc <= 2) {
		auto topts = std::make_shared<TargetOptions>();
		topts->Triple = triple;
		TargetInfo *ti = TargetInfo::CreateTargetInfo(diags, topts);
		if (!ti) { llvm::errs() << "no target info\n"; return 1; }
		LangOptions lo;
		std::string buf;
		llvm::raw_string_ostream os(buf);
		MacroBuilder mb(os);
		ti->getTargetDefines(lo, mb);
		llvm::outs() << os.str();
		llvm::outs() << "MCount " << ti->getTargetOpts().Triple
		    << " long double bits " << ti->getLongDoubleWidth() << "\n";
		return 0;
	}
	// dftest TRIPLE driver-args...: print the jobs.
	driver::Driver d("/usr/bin/clang", triple, diags);
	d.ResourceDir = "/res";
	std::vector<const char *> args(argv + 2, argv + argc);
	args.insert(args.begin(), "clang");
	std::unique_ptr<driver::Compilation> c(d.BuildCompilation(args));
	if (!c) return 1;
	c->getJobs().Print(llvm::outs(), "\n", true);
	return 0;
}
