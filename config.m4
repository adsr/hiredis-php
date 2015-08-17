PHP_ARG_WITH(hiredis, for hiredis support,
[  --with-hiredis             Include hiredis support])

if test "$PHP_HIREDIS" != "no"; then
  dnl
  dnl Find header files
  dnl
  SEARCH_PATH="/usr/local /usr"
  SEARCH_FOR="/include/hiredis/hiredis.h"
  if test -r $PHP_HIREDIS/$SEARCH_FOR; then
    HIREDIS_DIR=$PHP_HIREDIS
  else
    AC_MSG_CHECKING([for hiredis files in default path])
    for i in $SEARCH_PATH ; do
      if test -r $i/$SEARCH_FOR; then
        HIREDIS_DIR=$i
        AC_MSG_RESULT(found in $i)
      fi
    done
  fi
  if test -z "$HIREDIS_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Please install hiredis development files])
  fi
  PHP_ADD_INCLUDE($HIREDIS_DIR/include/hiredis)

  dnl
  dnl Check library
  dnl
  LIBNAME=hiredis
  LIBSYMBOL=redisCommandArgv
  PHP_CHECK_LIBRARY($LIBNAME,$LIBSYMBOL,
  [
    PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $HIREDIS_DIR/$PHP_LIBDIR, HIREDIS_SHARED_LIBADD)
    AC_DEFINE(HAVE_HIREDISLIB,1,[ ])
  ],[
    AC_MSG_ERROR([wrong hiredis lib version or lib not found])
  ],[
    -L$HIREDIS_DIR/$PHP_LIBDIR -lm
  ])
  PHP_SUBST(HIREDIS_SHARED_LIBADD)

  PHP_NEW_EXTENSION(hiredis, hiredis.c, $ext_shared)
fi
