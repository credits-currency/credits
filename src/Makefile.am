AM_CPPFLAGS =  $(INCLUDES)
AM_LDFLAGS = $(PTHREAD_CFLAGS)


if EMBEDDED_LEVELDB
LEVELDB_CPPFLAGS += -I$(srcdir)/leveldb/include
LEVELDB_CPPFLAGS += -I$(srcdir)/leveldb/helpers/memenv
LIBLEVELDB += $(builddir)/leveldb/libleveldb.a
LIBMEMENV  += $(builddir)/leveldb/libmemenv.a

# NOTE: This dependency is not strictly necessary, but without it make may try to build both in parallel, which breaks the LevelDB build system in a race
$(LIBLEVELDB): $(LIBMEMENV)

$(LIBLEVELDB) $(LIBMEMENV):
	@echo "Building LevelDB ..." && $(MAKE) -C $(@D) $(@F) CXX="$(CXX)" \
	  CC="$(CC)" PLATFORM=$(TARGET_OS) AR="$(AR)" $(LEVELDB_TARGET_FLAGS) \
          OPT="$(CXXFLAGS) $(CPPFLAGS)"
endif

CREDITS_INCLUDES=-I$(builddir) -I$(builddir)/obj $(BOOST_CPPFLAGS) $(LEVELDB_CPPFLAGS)

noinst_LIBRARIES = \
  libcredits_server.a \
  libcredits_common.a \
  libcredits_cli.a
if ENABLE_WALLET
CREDITS_INCLUDES += $(BDB_CPPFLAGS)
noinst_LIBRARIES += libcredits_wallet.a
endif

bin_PROGRAMS =
TESTS =

if BUILD_BITCOIND
  bin_PROGRAMS += creditsd
endif

if BUILD_BITCOIN_CLI
  bin_PROGRAMS += credits-cli
endif

.PHONY: FORCE
# bitcoin core #
BITCOIN_CORE_H = \
  addrman.h \
  alert.h \
  allocators.h \
  base58.h \
  bloom.h \
  chainparams.h \
  checkpoints.h \
  bitcoin_checkpoints.h \
  checkqueue.h \
  clientversion.h \
  coincontrol.h \
  coins.h \
  bitcoin_coins.h \
  compat.h \
  core.h \
  bitcoin_core.h \
  crypter.h \
  db.h \
  bitcoin_db.h \
  hash.h \
  init.h \
  key.h \
  keystore.h \
  leveldbwrapper.h \
  limitedmap.h \
  main_common.h \
  main.h \
  bitcoin_main.h \
  miner.h \
  mruset.h \
  netbase.h \
  net.h \
  noui.h \
  protocol.h \
  rpcclient.h \
  rpcprotocol.h \
  rpcserver.h \
  script.h \
  bitcoin_script.h \
  serialize.h \
  sync.h \
  subsidylevels.h \
  threadsafety.h \
  tinyformat.h \
  txdb.h \
  bitcoin_txdb.h \
  txmempool.h \
  bitcoin_txmempool.h \
  ui_interface.h \
  uint256.h \
  util.h \
  version.h \
  walletdb.h \
  bitcoin_walletdb.h \
  wallet.h \
  bitcoin_wallet.h

JSON_H = \
  json/json_spirit.h \
  json/json_spirit_error_position.h \
  json/json_spirit_reader.h \
  json/json_spirit_reader_template.h \
  json/json_spirit_stream_reader.h \
  json/json_spirit_utils.h \
  json/json_spirit_value.h \
  json/json_spirit_writer.h \
  json/json_spirit_writer_template.h

obj/build.h: FORCE
	@$(MKDIR_P) $(builddir)/obj
	@$(top_srcdir)/share/genbuild.sh $(abs_top_builddir)/src/obj/build.h \
	  $(abs_top_srcdir)
libcredits_common_a-version.$(OBJEXT): obj/build.h

libcredits_server_a_CPPFLAGS = $(CREDITS_INCLUDES)
libcredits_server_a_SOURCES = \
  addrman.cpp \
  alert.cpp \
  bloom.cpp \
  checkpoints.cpp \
  bitcoin_checkpoints.cpp \
  coins.cpp \
  bitcoin_coins.cpp \
  init.cpp \
  keystore.cpp \
  leveldbwrapper.cpp \
  main_common.cpp \
  main.cpp \
  bitcoin_main.cpp \
  miner.cpp \
  net.cpp \
  noui.cpp \
  rpcblockchain.cpp \
  rpcmining.cpp \
  rpcmisc.cpp \
  rpcnet.cpp \
  rpcrawtransaction.cpp \
  rpcserver.cpp \
  txdb.cpp \
  bitcoin_txdb.cpp \
  txmempool.cpp \
  bitcoin_txmempool.cpp \
  $(JSON_H) \
  $(BITCOIN_CORE_H)

libcredits_wallet_a_CPPFLAGS = $(CREDITS_INCLUDES)
libcredits_wallet_a_SOURCES = \
  db.cpp \
  bitcoin_db.cpp \
  crypter.cpp \
  rpcdump.cpp \
  rpcwallet.cpp \
  wallet.cpp \
  bitcoin_wallet.cpp \
  walletdb.cpp \
  bitcoin_walletdb.cpp \
  $(BITCOIN_CORE_H)

libcredits_common_a_CPPFLAGS = $(CREDITS_INCLUDES)
libcredits_common_a_SOURCES = \
  base58.cpp \
  allocators.cpp \
  chainparams.cpp \
  core.cpp \
  bitcoin_core.cpp \
  hash.cpp \
  key.cpp \
  netbase.cpp \
  protocol.cpp \
  rpcprotocol.cpp \
  script.cpp \
  bitcoin_script.cpp \
  sync.cpp \
  util.cpp \
  version.cpp \
  $(BITCOIN_CORE_H)

if GLIBC_BACK_COMPAT
libcredits_common_a_SOURCES += compat/glibc_compat.cpp
libcredits_common_a_SOURCES += compat/glibcxx_compat.cpp
endif

libcredits_cli_a_SOURCES = \
  rpcclient.cpp \
  $(BITCOIN_CORE_H)

nodist_libcredits_common_a_SOURCES = $(srcdir)/obj/build.h
#

# creditsd binary #
creditsd_LDADD = \
  libcredits_server.a \
  libcredits_cli.a \
  libcredits_common.a \
  $(LIBLEVELDB) \
  $(LIBMEMENV)
if ENABLE_WALLET
creditsd_LDADD += libcredits_wallet.a
endif
creditsd_SOURCES = creditsd.cpp
#

if TARGET_WINDOWS
creditsd_SOURCES += creditsd-res.rc
endif

creditsd_LDADD += $(BOOST_LIBS) $(BDB_LIBS)
creditsd_CPPFLAGS = $(CREDITS_INCLUDES)

# credits-cli binary #
credits_cli_LDADD = \
  libcredits_cli.a \
  libcredits_common.a \
  $(BOOST_LIBS)
credits_cli_SOURCES = credits-cli.cpp
credits_cli_CPPFLAGS = $(CREDITS_INCLUDES)
#

if TARGET_WINDOWS
credits_cli_SOURCES += credits-cli-res.rc
endif

CLEANFILES = leveldb/libleveldb.a leveldb/libmemenv.a *.gcda *.gcno

DISTCLEANFILES = obj/build.h

EXTRA_DIST = leveldb

clean-local:
	-$(MAKE) -C leveldb clean
	rm -f leveldb/*/*.gcno leveldb/helpers/memenv/*.gcno

.rc.o:
	@test -f $(WINDRES)
	$(AM_V_GEN) $(WINDRES) -i $< -o $@

.mm.o:
	$(AM_V_CXX) $(OBJCXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) \
	  $(CPPFLAGS) $(AM_CXXFLAGS) $(QT_INCLUDES) $(CXXFLAGS)  -c -o $@ $<

%.pb.cc %.pb.h: %.proto
	@test -f $(PROTOC)
	$(AM_V_GEN) $(PROTOC) --cpp_out=$(@D) --proto_path=$(abspath $(<D) $<)

LIBCREDITS_SERVER=libcredits_server.a
LIBCREDITS_WALLET=libcredits_wallet.a
LIBCREDITS_COMMON=libcredits_common.a
LIBCREDITS_CLI=libcredits_cli.a
LIBCREDITSQT=qt/libcreditsqt.a

if ENABLE_TESTS
include Makefile.test.include
endif

if ENABLE_QT
include Makefile.qt.include
endif

if ENABLE_QT_TESTS
include Makefile.qttest.include
endif
