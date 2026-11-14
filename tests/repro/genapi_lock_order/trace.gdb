set pagination off
set confirm off
set breakpoint pending on
break log4cpp_GenICam::Category::addAppender(log4cpp_GenICam::Appender&)
commands
silent
printf "\n=== addAppender caller stack ===\n"
bt 40
continue
end
break log4cpp_GenICam::Category::removeAllAppenders()
commands
silent
printf "\n=== removeAllAppenders caller stack ===\n"
bt 40
continue
end
run
