project(
	'sdb',
	'c',
	version: '0.1',
	default_options: [
		'c_std=gnu23',
		'warning_level=3',
	],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-Wconversion', language: 'c')

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
		'dictzip.c',
		'sdb.c',
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
