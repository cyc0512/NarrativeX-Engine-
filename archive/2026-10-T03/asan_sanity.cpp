#include <iostream>
int main() {
    int* p = new int[8];
    p[0] = 42;
    delete[] p;
    std::cout << "故意读已释放内存: " << p[0] << std::endl;   // 必然触发 heap-use-after-free
    return 0;
}
