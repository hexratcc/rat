#ifndef RAT_H
#define RAT_H

#include "ir/module.h"

#include "codegen/linear_scan_reg_alloc.h"
#include "codegen/schedule.h"
#include "target/target.h"

#include "pass/pass_manager.h"
#include "pass/pass_registry.h"
#include "pass/verify.h"

#include "pass/emit/c_emitter.h"
#include "pass/emit/graph_emitter.h"
#include "pass/emit/text_emitter.h"
#include "pass/emit/x86_encode.h"
#include "pass/emit/x86_layout.h"
#include "pass/emit/x86_lower.h"

#include "pass/opt/fold.h"
#include "pass/opt/gvn.h"
#include "pass/opt/inline.h"
#include "pass/opt/memory_opt.h"
#include "pass/opt/rename_symbol.h"
#include "pass/opt/sccp.h"
#include "pass/opt/simplify_cfg.h"

#endif
