EXTENSION = gp_replica_check
DATA = gp_replica_check--0.0.1.sql
MODULES = gp_replica_check
SCRIPTS = gp_replica_check.py

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/gp_replica_check
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

installcheck: install
	gp_replica_check.py -d=all -r=all
