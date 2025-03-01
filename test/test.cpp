#include <iostream>
#include <string>
#include "threadpool.h"

using namespace std;


int sum1(int a, int b) {
    this_thread::sleep_for(chrono::seconds(2)); // 模拟耗时
    return a + b;
}

int sum2(int a, int b, int c) {
    this_thread::sleep_for(chrono::seconds(2));
    return a + b + c;
}

string sum3(string a, string b) {
    this_thread::sleep_for(chrono::seconds(2));
    return a + b;
}

char fc() {
    this_thread::sleep_for(chrono::seconds(2));
    return '!';
}


int main() {
    // 开启线程池
    ThreadPool pool;
    // 设置线程池模式
    pool.setMode(PoolMode::MODE_CACHED);
    // 初始线程数量
    pool.start(2);

    future<int> r1 = pool.submitTask(sum1, 1, 2);
    future<int> r2 = pool.submitTask(sum2, 1, 2, 3);
    future<int> r3 = pool.submitTask([](int b, int e)->int {
            int sum = 0;
            for (int i = b; i <= e; i++) sum += i;
            return sum;
        }, 1, 100);
    future<string> r4 = pool.submitTask(&sum3, "ab", "cd");
    future<char> r5 = pool.submitTask(fc);

    cout << r1.get() << endl;
    cout << r2.get() << endl;
    cout << r3.get() << endl;
    cout << r4.get() << endl;
    cout << r5.get() << endl;
}