srcs_composite = [
	'compalloc.c',
	'compdirect.c',
	'compext.c',
	'compframe.c',
	'comphold.c',
	'compinit.c',
	'compoverlay.c',
	'compserver.c',
	'compsync.c',
	'compwindow.c',
]

hdrs_composite = [
	'compositeext.h',
]

libxserver_composite = static_library('libxserver_composite',
	srcs_composite,
	include_directories: inc,
	dependencies: common_dep,
)

if build_xorg
    install_data(hdrs_composite, install_dir: xorgsdkdir)
endif
