#include <iostream>
#include <fstream>
#include <vector>
int main() {
    std::ifstream ifs("d:/xiahold/XiahServerEx/build/Debug/MAP/0006.mm", std::ios::binary);
    if (!ifs) return 1;
    ifs.seekg(62, std::ios::beg);
    std::vector<char> buf(2048*2048);
    ifs.read(buf.data(), buf.size());
    int zeros = 0, ones = 0;
    for (char c : buf) {
        if (c == 0) zeros++;
        else if (c == 1) ones++;
    }
    std::cout << "Zeros: " << zeros << ", Ones: " << ones << std::endl;
    
    // Check pixel at 360, 620 (Spawn point)
    int y = 620; int x = 360;
    int bmpY = 2047 - y;
    std::cout << "At 360, 620 (bottom-up): " << (int)buf[bmpY * 2048 + x] << std::endl;
    std::cout << "At 360, 620 (top-down): " << (int)buf[y * 2048 + x] << std::endl;
    return 0;
}
