#ifndef HEADERS_H
#define HEADERS_H

#include "../include/vermell/vermell.h"
#include "../include/vermell/util/sysprocess.h"
#include "utils/min.http.h"
#include <future>
#include <string>
#include <gtest/gtest.h>

using std::string, std::future;
#define ISOLATE(logic) std::future<void> isolate_method = std::async(std::launch::async, ([&]() { logic }));

class TestSuite : public ::testing::Test {

  protected:

    static string expected_default;

     static void SetUpTestCase(){
        expected_default = "success";
     }
};

string TestSuite::expected_default;


#endif //HEADERS_H
