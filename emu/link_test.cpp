#include "unsp.h"
#include <cstdio>
int main(){ std::printf("link ok\n"); unsp_20_device *c = new unsp_20_device(); std::printf("iso=%d\n", c->m_iso); return 0; }
