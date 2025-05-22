project(
	'sdb',
	'c',
	version: '0.1',
	default_options: [
		'warning_level=3',
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
		'sdb.c',
	],
	link_with: [
		utf,
	],
	install : true,
)

install_man('sdb.1')
