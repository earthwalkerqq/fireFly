rm -rf ./build > /dev/null;
rm ./firefly > /dev/null;
mkdir -p ./build;
cd ./build;
cmake -S .. -B build -DUSE_OPENCL=ON
cmake --build build
cd ..;
./firefly;