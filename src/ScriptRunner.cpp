#include "ScriptRunner.h"
#include <QFile>
#include <QMainWindow>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>

#include <cmath>

const int ScriptRunner::c_physics_fps = 10;

ScriptRunner::ScriptRunner(QUrl url, QString script_file, bool debug, bool headless) :
    QObject(NULL),
    m_url(url),
    m_main_script_filename(script_file),
    m_debug(debug),
    m_headless(headless),
    m_engine(NULL),
    m_game(new Game(url)),
    m_started_game(false),
    m_exiting(false),
    m_stderr(stderr),
    m_stdout(stdout),
    m_timer_count(0),
    m_debugger(new QScriptEngineDebugger())
{
    m_engine = new QScriptEngine(this);

    if (m_debug)
        m_debugger->attachTo(m_engine);

    // run in our own thread
    m_thread = new QThread();
    m_thread->start();
    this->moveToThread(m_thread);
}

void ScriptRunner::go()
{
    if (QThread::currentThread() != m_thread) {
        bool success;
        success = QMetaObject::invokeMethod(this, "go", Qt::QueuedConnection);
        Q_ASSERT(success);
        return;
    }

    // initialize the MF object before we run any user code
    QScriptValue mf_obj = m_engine->newObject();
    m_engine->globalObject().setProperty("mf", mf_obj);

    // init event handler framework
    {
        QString file_name = ":/js/create_handlers.js";
        m_handler_map = evalJsonContents(internalReadFile(file_name), file_name);
    }
    // create JavaScript enums from C++ enums
    {
        QString enum_contents = internalReadFile(":/enums/ItemTypeEnum.h").trimmed();
        QStringList lines = enum_contents.split("\n");
        QStringList values = lines.takeFirst().split(" ");
        Q_ASSERT(values.size() == 2);
        QString prop_name = values.at(1);
        Q_ASSERT(values.at(0) == "enum");
        enum_contents = lines.join("\n");
        Q_ASSERT(enum_contents.endsWith(";"));
        enum_contents.chop(1);
        mf_obj.setProperty(prop_name, evalJsonContents(enum_contents.replace("=", ":")));
    }

    // add utility functions
    mf_obj.setProperty("include", m_engine->newFunction(include));
    mf_obj.setProperty("exit", m_engine->newFunction(exit));
    mf_obj.setProperty("print", m_engine->newFunction(print));
    mf_obj.setProperty("debug", m_engine->newFunction(debug));
    mf_obj.setProperty("setTimeout", m_engine->newFunction(setTimeout));
    mf_obj.setProperty("clearTimeout", m_engine->newFunction(clearTimeout));
    mf_obj.setProperty("setInterval", m_engine->newFunction(setInterval));
    mf_obj.setProperty("clearInterval", m_engine->newFunction(clearTimeout));
    mf_obj.setProperty("readFile", m_engine->newFunction(readFile));
    mf_obj.setProperty("writeFile", m_engine->newFunction(writeFile));

    // hook up mf functions
    mf_obj.setProperty("chat", m_engine->newFunction(chat));
    mf_obj.setProperty("username", m_engine->newFunction(username));
    mf_obj.setProperty("itemStackHeight", m_engine->newFunction(itemStackHeight));
    mf_obj.setProperty("health", m_engine->newFunction(health));
    mf_obj.setProperty("blockAt", m_engine->newFunction(blockAt));
    mf_obj.setProperty("playerState", m_engine->newFunction(playerState));
    mf_obj.setProperty("setControlState", m_engine->newFunction(setControlState));

    mf_obj.setProperty("Point", m_engine->newFunction(Point, 3));

    QString main_script_contents = internalReadFile(m_main_script_filename);
    if (main_script_contents.isNull()) {
        qWarning() << "file not found: " << m_main_script_filename;
        shutdown(1);
        return;
    }
    m_engine->evaluate(main_script_contents, m_main_script_filename);
    checkEngine("evaluating main script");

    if (m_exiting)
        return;

    // connect to server
    bool success;
    success = connect(m_game, SIGNAL(chunkUpdated(Int3D,Int3D)), this, SLOT(handleChunkUpdated(Int3D,Int3D)));
    Q_ASSERT(success);
    success = connect(m_game, SIGNAL(playerPositionUpdated()), this, SLOT(movePlayerPosition()));
    Q_ASSERT(success);
    success = connect(m_game, SIGNAL(loginStatusUpdated(Server::LoginStatus)), this, SLOT(handleLoginStatusUpdated(Server::LoginStatus)));
    Q_ASSERT(success);
    success = connect(m_game, SIGNAL(chatReceived(QString,QString)), this, SLOT(handleChatReceived(QString,QString)));
    Q_ASSERT(success);
    success = connect(m_game, SIGNAL(playerDied()), this, SLOT(handlePlayerDied()));
    Q_ASSERT(success);
    success = connect(m_game, SIGNAL(playerHealthUpdated()), this, SLOT(handlePlayerHealthUpdated()));
    Q_ASSERT(success);
    success = connect(&m_physics_timer, SIGNAL(timeout()), this, SLOT(doPhysics()));
    Q_ASSERT(success);

    m_started_game = true;
    m_game->start();
}

void ScriptRunner::doPhysics()
{
    float elapsed_time = m_physics_time.restart() / 1000.0f;
    m_game->doPhysics(elapsed_time);
}

QString ScriptRunner::internalReadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QString contents = QString::fromUtf8(file.readAll());
    file.close();
    return contents;
}

QScriptValue ScriptRunner::evalJsonContents(const QString & file_contents, const QString & file_name)
{
    QScriptValue value = m_engine->evaluate(QString("(") + file_contents + QString(")"), file_name);
    checkEngine("evaluating json");
    return value;
}

void ScriptRunner::checkEngine(const QString & while_doing_what)
{
    if (m_exiting)
        return;
    if (!m_engine->hasUncaughtException())
        return;
    if (m_engine->uncaughtException().toString() != "Error: SystemExit") {
        if (!while_doing_what.isEmpty())
            qWarning() << "Error while" << while_doing_what.toStdString().c_str();
        qWarning() << m_engine->uncaughtException().toString();
        qWarning() << m_engine->uncaughtExceptionBacktrace().join("\n");
    }
    shutdown(1);
}

void ScriptRunner::dispatchTimeout()
{
    QTimer * timer = (QTimer *) sender();
    TimedFunction tf = m_script_timers.value(timer);
    if (! tf.repeat) {
        m_timer_ptrs.remove(tf.id);
        m_script_timers.remove(timer);
        delete timer;
    }
    tf.function.call(tf.this_ref);
}

QScriptValue ScriptRunner::setTimeout(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 2))
        return error;
    QScriptValue func = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, func.isFunction()))
        return error;
    QScriptValue ms = context->argument(1);
    if (!me->maybeThrowArgumentError(context, error, ms.isNumber()))
        return error;

    return me->setTimeout(func, ms.toInteger(), context->parentContext()->thisObject(), false);
}

QScriptValue ScriptRunner::setInterval(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 2))
        return error;
    QScriptValue func = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, func.isFunction()))
        return error;
    QScriptValue ms = context->argument(1);
    if (!me->maybeThrowArgumentError(context, error, ms.isNumber()))
        return error;

    return me->setTimeout(func, ms.toInteger(), context->parentContext()->thisObject(), true);
}

int ScriptRunner::setTimeout(QScriptValue func, int ms, QScriptValue this_obj, bool repeat)
{
    QTimer * new_timer = new QTimer(this);
    new_timer->setInterval(ms);
    bool success;
    success = connect(new_timer, SIGNAL(timeout()), this, SLOT(dispatchTimeout()));
    Q_ASSERT(success);
    int timer_id = nextTimerId();
    m_timer_ptrs.insert(timer_id, new_timer);
    m_script_timers.insert(new_timer, TimedFunction(timer_id, repeat, this_obj, func));
    new_timer->start();
    return timer_id;
}

QScriptValue ScriptRunner::clearTimeout(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 2))
        return error;
    QScriptValue id = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, id.isNumber()))
        return error;

    int int_id = id.toInteger();
    QTimer * ptr = me->m_timer_ptrs.value(int_id, NULL);
    me->m_timer_ptrs.remove(int_id);
    me->m_script_timers.remove(ptr);
    delete ptr;
    return QScriptValue();
}

QScriptValue ScriptRunner::readFile(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 1))
        return error;
    QScriptValue path_value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, path_value.isString()))
        return error;

    QString path = path_value.toString();
    QString contents = me->internalReadFile(path);
    if (contents.isNull())
        return QScriptValue();
    return contents;
}
QScriptValue ScriptRunner::writeFile(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 2))
        return error;
    QScriptValue path_value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, path_value.isString()))
        return error;
    QScriptValue contents_value = context->argument(1);
    if (!me->maybeThrowArgumentError(context, error, contents_value.isString()))
        return error;

    QString path = path_value.toString();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return context->throwError(tr("Unable to write file: %1").arg(path));
    QByteArray contents = contents_value.toString().toUtf8();
    int index = 0;
    while (index < contents.size()) {
        int written_size = file.write(contents.mid(index));
        if (written_size == -1) {
            file.close();
            return context->throwError(tr("Unable to write file: %1").arg(path));
        }
        index += written_size;
    }
    file.close();
    return QScriptValue();
}

int ScriptRunner::nextTimerId()
{
    return m_timer_count++;
}

void ScriptRunner::raiseEvent(QString event_name, const QScriptValueList &args)
{
    QScriptValue handlers_value = m_handler_map.property(event_name);
    Q_ASSERT(handlers_value.isArray());
    int length = handlers_value.property("length").toInt32();
    if (length == 0)
        return;
    // create copy so handlers can remove themselves
    QList<QScriptValue> handlers_list;
    for (int i = 0; i < length; i++)
        handlers_list.append(handlers_value.property(i));
    foreach (QScriptValue handler, handlers_list) {
        Q_ASSERT(handler.isFunction());
        handler.call(QScriptValue(), args);
        checkEngine(QString("calling event handler") + event_name);
    }
}

QScriptValue ScriptRunner::include(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (! me->argCount(context, error, 1))
        return error;
    QScriptValue file_name_value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, file_name_value.isString()))
        return error;
    QString file_name = file_name_value.toString();

    QString absolute_name = QFileInfo(me->m_main_script_filename).dir().absoluteFilePath(file_name);
    QString contents = me->internalReadFile(absolute_name);
    if (contents.isNull())
        return context->throwError(tr("Connot open included file: %1").arg(absolute_name));
    context->setActivationObject(engine->globalObject());
    context->setThisObject(engine->globalObject());
    engine->evaluate(contents, file_name);
    me->checkEngine("evaluating included file");
    return QScriptValue();
}

QScriptValue ScriptRunner::Point(QScriptContext *context, QScriptEngine *engine)
{
    // TODO: replace this method with a js Point class
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (! me->argCount(context, error, 3))
        return error;

    QScriptValue pt = engine->newObject();
    pt.setProperty("x", context->argument(0));
    pt.setProperty("y", context->argument(1));
    pt.setProperty("z", context->argument(2));
    return pt;
}

QScriptValue ScriptRunner::exit(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (! me->argCount(context, error, 0, 1))
        return error;
    int return_code = 0;
    if (context->argumentCount() == 1) {
        QScriptValue return_code_value = context->argument(0);
        if (!me->maybeThrowArgumentError(context, error, return_code_value.isNumber()))
            return error;
        return_code = return_code_value.toInt32();
    }
    // TODO: care about the return code
    return context->throwError("SystemExit");
}

QScriptValue ScriptRunner::print(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 1))
        return error;
    QScriptValue arg_value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, arg_value.isString()))
        return error;
    QString text = arg_value.toString();
    me->m_stdout << text;
    me->m_stdout.flush();
    return QScriptValue();
}

QScriptValue ScriptRunner::debug(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (! me->argCount(context, error, 1))
        return error;
    // argument can be anything
    QString line = context->argument(0).toString();
    me->m_stderr << line << "\n";
    me->m_stderr.flush();
    return QScriptValue();
}

void ScriptRunner::shutdown(int return_code)
{
    m_exiting = true;
    if (m_started_game) {
        m_thread->exit(return_code);
        m_game->shutdown(return_code);
    } else {
        QCoreApplication::exit(return_code);
    }
}

QScriptValue ScriptRunner::chat(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 1))
        return error;
    QScriptValue message_value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, message_value.isString()))
        return error;
    QString message = message_value.toString();
    me->m_game->sendChat(message);
    return QScriptValue();
}

QScriptValue ScriptRunner::username(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 0))
        return error;
    return me->m_url.userName();
}

QScriptValue ScriptRunner::itemStackHeight(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 1))
        return error;
    QScriptValue value = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, value.isNumber()))
        return error;
    return me->m_game->itemStackHeight((Block::ItemType)value.toInteger());
}

QScriptValue ScriptRunner::health(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 0))
        return error;
    return me->m_game->playerHealth();
}

QScriptValue ScriptRunner::blockAt(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 1))
        return error;
    QScriptValue js_pt = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, js_pt.isObject()))
        return error;
    Int3D pt(valueToNearestInt(js_pt.property("x").toNumber()),
             valueToNearestInt(js_pt.property("y").toNumber()),
             valueToNearestInt(js_pt.property("z").toNumber()));
    Block block = me->m_game->blockAt(pt);
    QScriptValue result = engine->newObject();
    result.setProperty("type", block.type());
    return result;
}

QScriptValue ScriptRunner::playerState(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 0))
        return error;
    Server::EntityPosition position = me->m_game->playerPosition();
    QScriptValue result = engine->newObject();
    result.setProperty("position", me->jsPoint(position.x, position.y, position.z));
    result.setProperty("velocity", me->jsPoint(position.dx, position.dy, position.dz));
    result.setProperty("yaw", position.yaw);
    result.setProperty("pitch", position.pitch);
    result.setProperty("on_ground", position.on_ground);
    return result;
}

QScriptValue ScriptRunner::setControlState(QScriptContext *context, QScriptEngine *engine)
{
    ScriptRunner * me = (ScriptRunner *) engine->parent();
    QScriptValue error;
    if (!me->argCount(context, error, 2))
        return error;
    QScriptValue control = context->argument(0);
    if (!me->maybeThrowArgumentError(context, error, control.isNumber()))
        return error;
    QScriptValue state = context->argument(1);
    if (!me->maybeThrowArgumentError(context, error, state.isBool()))
        return error;

    me->m_game->setControlActivated((Game::Control) control.toInteger(), state.toBool());
    return QScriptValue();
}

int ScriptRunner::valueToNearestInt(const QScriptValue &value)
{
    return (int)std::floor(value.toNumber() + 0.5);
}

bool ScriptRunner::argCount(QScriptContext *context, QScriptValue &error, int arg_count_min, int arg_count_max)
{
    if (arg_count_max == -1)
        arg_count_max = arg_count_min;
    if (arg_count_min <= context->argumentCount() && context->argumentCount() <= arg_count_max)
        return true;

    QString message;
    if (arg_count_min == arg_count_max)
        message = tr("Expected %1 arguments. Received %2").arg(arg_count_min, context->argumentCount());
    else
        message = tr("Expected between %1 and %2 arguments. Received %3").arg(arg_count_min, arg_count_max, context->argumentCount());
    error = context->throwError(message);
    return false;
}

bool ScriptRunner::maybeThrowArgumentError(QScriptContext *context, QScriptValue &error, bool arg_is_valid)
{
    if (arg_is_valid)
        return true;
    error = context->throwError("Invalid Argument");
    return false;
}

QScriptValue ScriptRunner::jsPoint(const Int3D &pt)
{
    QScriptValue obj = m_engine->newObject();
    obj.setProperty("x", pt.x);
    obj.setProperty("y", pt.y);
    obj.setProperty("z", pt.z);
    return obj;
}

QScriptValue ScriptRunner::jsPoint(double x, double y, double z)
{
    QScriptValue obj = m_engine->newObject();
    obj.setProperty("x", x);
    obj.setProperty("y", y);
    obj.setProperty("z", z);
    return obj;
}

void ScriptRunner::handleChunkUpdated(const Int3D &start, const Int3D &size)
{
    raiseEvent("onChunkUpdated", QScriptValueList() << jsPoint(start) << jsPoint(size));
}

void ScriptRunner::movePlayerPosition()
{
    raiseEvent("onPositionUpdated");
}

void ScriptRunner::handlePlayerHealthUpdated()
{
    raiseEvent("onHealthChanged");
}

void ScriptRunner::handlePlayerDied()
{
    raiseEvent("onDeath");
}

void ScriptRunner::handleChatReceived(QString username, QString message)
{
    raiseEvent("onChat", QScriptValueList() << username << message);
}

void ScriptRunner::handleLoginStatusUpdated(Server::LoginStatus status)
{
    // note that game class already handles shutting down for Disconnected and SocketError.
    switch (status) {
        case Server::Success:
            m_physics_time.start();
            doPhysics();
            m_physics_timer.start(1000 / c_physics_fps);
            raiseEvent("onConnected");
            break;
        default:;
    }
}
