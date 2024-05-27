North Carolina State University - ECE 566 - Compiler Optimization and Scheduling

High Level Overview 

o This program analyzes functions that are eligible for inlining, puts them in a worklist, and performs the inline as it navigates through the worklist. Specific requirements for inling could be set, such as constant arguments, growth factor, and function size limits.
o A report was compiled that analyzed the benefits of inlining across numerous benchmarks. Some benchmarks benefitted, while others performed worse with function inlining.

File Descriptions

P3.cpp - main cpp file that contains the identification of eligible functions. The inline function itself was created by the professor, James Tuck.
