SUBDIRS = RGBLut JoinViews OneView Anaglyph MixViews SideBySide
ifneq ($(DEBUGFLAG),-O3)
  # DebugProxy is only useful to debug the communication between a host and a plugin
  SUBDIRS += DebugProxy
  # ReConverge is not tested yet
  SUBDIRS += ReConverge
endif

all: subdirs

.PHONY: subdirs clean $(SUBDIRS)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean :
	for i in $(SUBDIRS) ; do \
	  $(MAKE) -C $$i clean; \
	done
