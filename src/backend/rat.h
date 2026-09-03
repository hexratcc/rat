#ifndef RAT_H
#define RAT_H

#include "ir/module.h"

#include "codegen/linear_scan_reg_alloc.h"
#include "target/target.h"

#include "pass/pass_manager.h"
#include "pass/pass_registry.h"
#include "pass/verify.h"

#include "pass/emit/text_emitter.h"
#include "pass/emit/x86/x86_encode.h"
#include "pass/emit/x86/x86_layout.h"
#include "pass/emit/x86/x86_lower.h"
#include "pass/emit/x86/x86_peephole.h"

#include "pass/opt/rename_symbol.h"

#endif
