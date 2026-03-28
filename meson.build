project(
	'sdb',
	'c',
	'cpp',
	version: '0.1',
	default_options: [
		'buildtype=debugoptimized',
		'warning_level=3',
		'cpp_std=c++26',
	],
)

utf = static_library(
	'utf',
	sources: [
		'utf/rune.c',
		'utf/runeistype.c',
		'utf/runetotype.c',
		'utf/utfnlen.c',
	],
	pic: false,
)

sdb = executable(
	'sdb',
	sources: [
		'sdb.cc',
	],
	link_with: [
		utf,
	],
	dependencies: [
		dependency('zlib-ng', required: true),
	],
	install : true,
)

install_man('sdb.1')
