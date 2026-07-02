CONTIKI_PROJECT = operator nodes
all: $(CONTIKI_PROJECT)

# Removed buffer.c and helpers.c from PROJECT_SOURCEFILES

CONTIKI = $(HOME)/contiki-ng

# Force standard CSMA and NullNet
MAKE_MAC = MAKE_MAC_CSMA
MAKE_NET = MAKE_NET_NULLNET

CFLAGS += -DPROJECT_CONF_H=\"project-conf.h\"

include $(CONTIKI)/Makefile.include
