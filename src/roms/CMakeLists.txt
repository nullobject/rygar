fips_begin_lib(roms)
  fipsutil_embed(rygar-roms.yml rygar-roms.h)
  fips_files(dummy.c)
fips_end_lib()
