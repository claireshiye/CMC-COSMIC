AC_DEFUN([ADD_FLAGS_PKG_CONF], [
	PKG_CHECK_MODULES(AS_TR_CPP([$1]), [$1])
	CPPFLAGS="$CPPFLAGS [$]AS_TR_CPP([$1])_CFLAGS"
	LDFLAGS="[$]AS_TR_CPP([$1])_LIBS $LDFLAGS"
	AC_MSG_NOTICE([CPPFLAGS expanded to $CPPFLAGS])
	AC_MSG_NOTICE([LDFLAGS expanded to $LDFLAGS])
])
