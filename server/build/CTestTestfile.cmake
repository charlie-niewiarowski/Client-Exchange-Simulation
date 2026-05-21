# CMake generated Testfile for 
# Source directory: /home/cniew/CLionProjects/ToyExchange/server
# Build directory: /home/cniew/CLionProjects/ToyExchange/server/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[TestValidation]=] "/home/cniew/CLionProjects/ToyExchange/server/build/TestValidation")
set_tests_properties([=[TestValidation]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;37;add_test;/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;0;")
add_test([=[TestServerPipeline]=] "/home/cniew/CLionProjects/ToyExchange/server/build/TestServerPipeline")
set_tests_properties([=[TestServerPipeline]=] PROPERTIES  RESOURCE_LOCK "tcp4000" _BACKTRACE_TRIPLES "/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;46;add_test;/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;0;")
add_test([=[TestIntegration]=] "/home/cniew/CLionProjects/ToyExchange/server/build/TestIntegration")
set_tests_properties([=[TestIntegration]=] PROPERTIES  RESOURCE_LOCK "tcp4000" _BACKTRACE_TRIPLES "/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;55;add_test;/home/cniew/CLionProjects/ToyExchange/server/CMakeLists.txt;0;")
