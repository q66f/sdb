@test a:
    ninja -Cb
    b/sdb -den {{a}}
@val a:
    ninja -Cb
    valgrind --leak-check=full --track-origins=yes -q b/sdb {{a}}
