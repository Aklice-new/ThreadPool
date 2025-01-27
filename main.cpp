#include "ThreadPool.h"
#include <thread>
#include <chrono>

using namespace std;

int sum1(int a, int b)
{
    int sum = 0;
    for (int i = a; i < b; i++)
    {
        sum += i;
    }
    return sum;
}

void sleep(int seconds)
{
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
}

int main()
{
    {
        ThreadPool pool;
        pool.start(4);

        auto num1 = pool.submitTask(sum1, 1, 1000000);
        auto num2 = pool.submitTask(sum1, 2, 100000000);
        pool.submitTask(sleep, 3);
        pool.submitTask(sleep, 3);
        pool.submitTask(sleep, 3);
        pool.submitTask(sleep, 3);
        pool.submitTask(sleep, 3);
    }

    std::cout << "Main func exit!" << std::endl;
    return 0;
}