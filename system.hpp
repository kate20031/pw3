#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <queue>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <ctime>

#include "machine.hpp"

using namespace std::chrono_literals;
typedef unsigned int OrderId;
typedef unsigned int WorkerId;
static std::vector<std::string> menu;
static std::queue<OrderId> orders;
static std::map<OrderId, bool> readyOrders;
static std::condition_variable cv2;
static bool working;
static std::mutex mutex;

class FulfillmentFailure : public std::exception
{
  const char* what() const throw () {
    return "FulfillmentFailure";
  }
};

class OrderNotReadyException : public std::exception
{
  const char* what() const throw () {
    return "OrderNotReadyException";
  }
};

class BadOrderException : public std::exception
{
  const char* what() const throw () {
    return "BadOrderException";
  }
};

class BadPagerException : public std::exception
{
  const char* what() const throw () {
    return "BadPagerException";
  }
};

class OrderExpiredException : public std::exception
{
  const char* what() const throw () {
    return "OrderExpiredException";
  }
};

class RestaurantClosedException : public std::exception
{
  const char* what() const throw () {
    return "RestaurantClosedException";
  }
};

struct WorkerReport
{
  std::vector<std::vector<std::string>> collectedOrders;
  std::vector<std::vector<std::string>> abandonedOrders;
  std::vector<std::vector<std::string>> failedOrders;
  std::vector<std::string> failedProducts;
};

class CoasterPager
{

public:
  CoasterPager(std::vector<std::basic_string<char>> products, unsigned int numberOfOrders)
      : products(std::move(products)) {
    id = numberOfOrders;
  };

  void wait() const {
    std::unique_lock lk(mutex);
    cv2.wait(lk, [this]{return isReady() || !working;});
    lk.unlock();
  }

  void wait(unsigned int timeout) const {
    std::chrono::system_clock::time_point time_passed
        = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout);

    std::unique_lock lk(mutex);
    cv2.wait_until(lk, time_passed, [this]{return isReady() || !working;});
    lk.unlock();

  }

  [[nodiscard]] unsigned int getId() const {
    return id;
  }

  [[nodiscard]] bool isReady() const {
    return readyOrders[id];
  }

private:
  OrderId id;
  std::vector<std::basic_string<char>> products;
};

class System {
  typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
  typedef std::vector<std::string> Order;

public:
  std::map <OrderId , WorkerId> orderAndWorker;

  System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout) :
  machines(machines), numberOfWorkers(numberOfWorkers), clientTimeout(clientTimeout)

  {
    working = true;

    for (auto & machine : machines) {
      brokenMachine[machine.first] = false;
      machine.second->start();
    }

    for(auto const&machine : machines) {
      menu.push_back(machine.first);
    }

    for (int i = 1; i <= numberOfWorkers; i++) {
      workers[i] = std::thread([this, i]() { work(i);});
      busyWorkers[i] = false;
    }
  }

  bool timeOut(OrderId id) {
    auto collectingTime = std::chrono::system_clock::now();

    auto diff = collectingTime - orderTime[id];
    auto diffInMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();

    return diffInMilliseconds > getClientTimeout();
  }

  std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    std::vector<std::unique_ptr<Product>> products;
    auto orderId = CoasterPager->getId();

    std::unique_lock lk(mutex);
    WorkerId workerId = orderAndWorker[orderId];
    busyWorkers[workerId] = false;
    workerWaitForCollecting.notify_all();
    lk.unlock();

    // Restaurant is closed.
    if (!working) {
      throw RestaurantClosedException();
    }

    // Machine was broken.
    if (fulfillmentFailure[orderId]) {
      throw FulfillmentFailure();
    }

    // Too late to collect order.
    if (timeOut(orderId)) {
      throw OrderExpiredException();
    }

    // Order can't be collected.
    if (!readyOrders.contains(orderId)) {
      throw BadPagerException();
    }

    // Order isn't ready yet.
    if (!readyOrders[orderId]) {
      throw OrderNotReadyException();
    }

    lk.lock();

    for (auto &pr : readyOrdersMap[orderId]) {
      products.push_back(std::move(pr.second));
    }

    reports[workerId].collectedOrders.push_back(ordersMap[orderId]);
    readyOrders.erase(orderId);

    lk.unlock();

    return products;

  }

  static void removeFromMenu(const std::string& product) {
    menu.erase(std::find(menu.begin(), menu.end(), product));
  }

  std::vector<std::string> getMenu() const {
    return menu;
  }

  std::vector<unsigned int> getPendingOrders() const {
    std::vector<unsigned int> result;

    for (const auto &order : readyOrders) {
      result.push_back(order.first);
    }

    return result;
  }

  unsigned int getClientTimeout() const {
    return clientTimeout;
  }

  std::vector<WorkerReport> shutdown() {
    std::vector<WorkerReport> report;

    std::unique_lock lk(mutex);
    working = false;
    menu.clear();
    lk.unlock();

    for (auto & machine : machines) {
      machine.second->stop();
    }

    cv.notify_all();

    for (int i = 1; i <= numberOfWorkers; i++) {
      workers[i].join();
    }


    for (auto & it : reports) {
      report.push_back(it.second);
    }

    return report;
  }

  std::unique_ptr<CoasterPager> order(std::vector<std::string> products) {
    if (!working) {
      throw RestaurantClosedException();
    }

    //The order is empty.
    if (products.empty()) {
      throw BadOrderException();
    }

    // There is no such product in menu.
    for (const auto &product: products) {
      if (std::find(menu.begin(), menu.end(), product) == menu.end()) {
        throw BadOrderException();
      }
    }

    std::unique_lock lk(mutex);
    numberOfOrders++;
    printf("Zamawiam %d   %s  %d\n", numberOfOrders, products.front().c_str(), working);
    std::unique_ptr<CoasterPager> ptr = std::make_unique<CoasterPager>(products, numberOfOrders);

    orders.push(numberOfOrders);
    ordersMap.insert({numberOfOrders, products});
    readyOrders[numberOfOrders] = false;
    fulfillmentFailure[numberOfOrders] = false;
    lk.unlock();
    cv.notify_one();

    return ptr;
  }

private:
  void handleBrokenMachine(OrderId orderId, WorkerId id, const std::string& productName, const Order &order) {
     reports[id].failedOrders.push_back(order);
     reports[id].failedProducts.push_back(productName);
     fulfillmentFailure[orderId] = true;
     brokenMachine[productName] = true;

     printf("Broken %d  on product %s size %ld\n", orderId, productName.c_str(), readyOrdersMap[orderId].size());
     for (auto &p : readyOrdersMap[orderId]) {
       if (p.second) {
         machines[p.first]->returnProduct(std::move(p.second));
       }
     }

     readyOrders.erase(orderId);
     removeFromMenu(productName);
   }

  Order chooseOrder(WorkerId id, OrderId orderId) {
    busyWorkers[id] = true;
    auto order = ordersMap.at(orderId);
    orders.pop();
    printf("Worker %d dostał zamówienie %d    %s\n ", id, orderId,
           ordersMap[orderId].front().c_str());
    orderAndWorker.insert({orderId, id});

    return order;
  }

  void addReadyOrder(OrderId orderId) {
    orderTime.insert({orderId, std::chrono::system_clock::now()});
    readyOrders[orderId] = true;
    cv2.notify_all();
  }

  void checkTimeOut(WorkerId id, OrderId orderId, const Order &order) {
    if (timeOut(orderId)) {
      reports[id].abandonedOrders.push_back(order);
      printf("Return %d size %ld\n", orderId, readyOrdersMap[orderId].size());

      for (auto &p : readyOrdersMap[orderId]) {
        if (p.second) {
          machines[p.first]->returnProduct(std::move(p.second));
        }
      }
      readyOrdersMap.erase(orderId);
      readyOrders.erase(orderId);
    }
  }

  void workAdd(WorkerId id) {
    while (working) {
      waitForOrder(id);
      if (!working) {
        break;
      }
      if (!orders.empty()) {
        handleOrder(id);
      }
    }
  }

  void waitForOrder(WorkerId id) {
    std::unique_lock lk(mutex);
    cv.wait(lk, [&id, this] {
      return (!busyWorkers[id] && !orders.empty()) || !working;
    });
  }

  void handleOrder(WorkerId id) {
    std::unique_lock lk(mutex);
    OrderId orderId = orders.front();
    auto order = chooseOrder(id, orderId);
    lk.unlock();

    processOrder(id, orderId, order);

    waitForCollecting(id, orderId, order);
  }

//  std::shared_ptr<Product> makeProduct(std::string type) {
//    std::shared_ptr<Machine> machine;
//    auto machineIt = machines.at(type);
//    auto product = machine->make();;
//
//    return product;
//  }

  void processOrder(WorkerId id, OrderId orderId, std::vector<std::string> &order) {
    for (auto it = order.begin(); it < order.end(); it++) {
      std::string productName = *it;
      auto machine = machines.at(productName);
      std::unique_ptr<Product> product;
      std::vector<std::future<std::string>> futures;

      try {
        product = std::move(machine->getProduct());
        handleProduct(id, orderId, productName, product, order);
      } catch (MachineFailure failure) {
        handleBrokenMachine(orderId, id, productName, order);
        break;
      } catch (MachineNotWorking mutex) {
        handleNotWorkingMachine(orderId, id);
        break;
      }
    }

    addReadyOrder(orderId);
  }

  void handleProduct(WorkerId id, OrderId orderId, std::string &productName,
                     std::unique_ptr<Product> &product, std::vector<std::string> &order) {
    if (brokenMachine[productName]) {
      handleBrokenMachine(orderId, id, productName, order);
      return;
    }
    readyOrdersMap[orderId].insert({productName, std::move(product)});
  }

  void handleNotWorkingMachine(OrderId orderId, WorkerId id) {
    fulfillmentFailure[orderId] = true;
    readyOrders.erase(orderId);
  }

  void waitForCollecting(WorkerId id, OrderId orderId, std::vector<std::string> &order) {
    std::unique_lock lk(mutex);
    workerWaitForCollecting.wait(lk, [&id, this, &orderId] {
      return (!busyWorkers[id] || timeOut(orderId) || !working);
    });
    checkTimeOut(id, orderId, order);
    lk.unlock();
  }

  void work(unsigned int id) {
    while (working) {
      workAdd(id);
    }
  }

  typedef  std::map<unsigned int, Order> OrderMap;
  unsigned int numberOfWorkers;
  machines_t machines;
  std::map<std::string, bool> brokenMachine;
  std::thread endOfWork;
  std::map<WorkerId , std::thread> workers;
  std::map<WorkerId, bool> busyWorkers;
  unsigned int numberOfOrders = 0;
  unsigned int clientTimeout;
  OrderMap ordersMap;
  std::map<OrderId, bool>  fulfillmentFailure;
  std::vector<OrderId>  orderExpired;

  std::map<OrderId, std::map<std::string, std::unique_ptr<Product>>> readyOrdersMap;
  std::map<OrderId , std::chrono::system_clock::time_point> orderTime;

  std::condition_variable cv;
  std::condition_variable workerWaitForCollecting;
  std::map<WorkerId, WorkerReport> reports;
};


#endif // SYSTEM_HPP