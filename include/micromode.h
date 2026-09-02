/**
 * @file micromode.h
 * @brief LF modal models for reactor-uc, implemented as a client of the generic
 * `LfExtension` mechanism. The runtime owns a gate; this library owns the policy.
 *
 * Built only when the `LF_MODAL_MODELS` option is on, which also requires reactor-uc's
 * `LF_RUNTIME_EXTENSIONS`.
 */
#ifndef MICROMODE_MICROMODE_H
#define MICROMODE_MICROMODE_H

#include "reactor-uc/error.h"


/** @brief Returns LF_OK. Exists so a test can prove the library was compiled and linked. */
lf_ret_t lf_micromode_abi_check(void);

#endif /* MICROMODE_MICROMODE_H */
