#ifndef GENERATOR_H
#define GENERATOR_H

#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include <cJSON/cJSON.h>

IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec);

#endif // GENERATOR_H
