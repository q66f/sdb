typedef struct Dictzip Dictzip;
void dzclose(Dictzip *dz);
Dictzip *dzopen(const char *path);
s64 dzread(Dictzip *dz, char *buf, u32 nbytes, u32 seek);

