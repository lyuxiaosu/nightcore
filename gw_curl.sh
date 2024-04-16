curl -w "Total time: %{time_total}s\n" -o /dev/null -s -X POST http://10.10.1.1:8080/function/FooG http://10.10.1.1:8080/function/FooG
