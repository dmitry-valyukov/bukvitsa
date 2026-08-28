#pragma once

#include <cassert>

// Импорт последним — правило проекта.
import wxl.async;
import std;

namespace bukvitsa::io {

// Куски схемы, доросшие до библиотеки, живут теперь в wxl.async. Здесь у них
// короткий адрес: в песочнице они пишутся как io::task, io::cancellation_token
// и так далее — так же, как писались, пока лежали рядом.
using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::operation_canceled_exception;
using wxl::async::spsc_channel;
using wxl::async::task;

}  // namespace bukvitsa::io
