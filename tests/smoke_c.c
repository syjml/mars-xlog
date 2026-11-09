#include <mars/xlog/xlogger.h>

void xlog_smoke_write_from_c(void) {
    xinfo2("C logging literal");
    xinfo2_if(1, "C formatted integer %d", 42);
}
