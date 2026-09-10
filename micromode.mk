# Build fragment for a Makefile-driven platform. The counterpart of CMakeLists.txt, for a
# platform that has no CMake to reuse micromode's target through.
#
# A generated project's Makefile names this file in LFC_GEN_EXTENSION_MKS. Including 
# it appends what micromode needs to the LF_EXTENSION_* lists a platform fragment
# consumes, so no platform has to know micromode by name.

MICROMODE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

LF_EXTENSION_SOURCES += $(wildcard $(MICROMODE_DIR)/src/*.c)
LF_EXTENSION_INCLUDES += $(MICROMODE_DIR)/include

LF_EXTENSION_CFLAGS += -DLF_RUNTIME_EXTENSIONS -DLF_MODAL_MODELS
