#ifndef KEYMAP_MCP_H
#define KEYMAP_MCP_H

#include <QString>
#include <QStringList>
#include <QMap>
#include "keymap.h"

struct KeyPosition {
    int row;
    int col;
    bool valid;
};

inline KeyPosition getKeyPosition(const QString &keyName)
{
    // Map of key names to keymap enum values
    static const QMap<QString, int> keyNameMap = {
        // Row 0
        {QStringLiteral("ret"), keymap::ret},
        {QStringLiteral("enter"), keymap::enter},
        {QStringLiteral("negative"), keymap::neg},
        {QStringLiteral("neg"), keymap::neg},
        {QStringLiteral("space"), keymap::space},
        {QStringLiteral("z"), keymap::az},
        {QStringLiteral("y"), keymap::ay},
        {QStringLiteral("0"), keymap::n0},
        {QStringLiteral("punct"), keymap::punct},
        {QStringLiteral("."), keymap::punct},
        {QStringLiteral("on"), keymap::on},

        // Row 1
        {QStringLiteral("x"), keymap::ax},
        {QStringLiteral("w"), keymap::aw},
        {QStringLiteral("v"), keymap::av},
        {QStringLiteral("3"), keymap::n3},
        {QStringLiteral("u"), keymap::au},
        {QStringLiteral("t"), keymap::at},
        {QStringLiteral("s"), keymap::as},
        {QStringLiteral("1"), keymap::n1},
        {QStringLiteral("pi"), keymap::pi},
        {QStringLiteral("trig"), keymap::trig},
        {QStringLiteral("10x"), keymap::pow10},
        {QStringLiteral("pow10"), keymap::pow10},

        // Row 2
        {QStringLiteral("r"), keymap::ar},
        {QStringLiteral("q"), keymap::aq},
        {QStringLiteral("p"), keymap::ap},
        {QStringLiteral("6"), keymap::n6},
        {QStringLiteral("o"), keymap::ao},
        {QStringLiteral("n"), keymap::an},
        {QStringLiteral("m"), keymap::am},
        {QStringLiteral("4"), keymap::n4},
        {QStringLiteral("ee"), keymap::ee},
        {QStringLiteral("exp"), keymap::ee},
        {QStringLiteral("square"), keymap::squ},
        {QStringLiteral("squ"), keymap::squ},

        // Row 3
        {QStringLiteral("l"), keymap::al},
        {QStringLiteral("k"), keymap::ak},
        {QStringLiteral("j"), keymap::aj},
        {QStringLiteral("9"), keymap::n9},
        {QStringLiteral("i"), keymap::ai},
        {QStringLiteral("h"), keymap::ah},
        {QStringLiteral("g"), keymap::ag},
        {QStringLiteral("7"), keymap::n7},
        {QStringLiteral("divide"), keymap::div},
        {QStringLiteral("div"), keymap::div},
        {QStringLiteral("/"), keymap::div},
        {QStringLiteral("exponent"), keymap::exp},
        {QStringLiteral("^"), keymap::exp},

        // Row 4
        {QStringLiteral("f"), keymap::af},
        {QStringLiteral("e"), keymap::ae},
        {QStringLiteral("d"), keymap::ad},
        {QStringLiteral("c"), keymap::ac},
        {QStringLiteral("b"), keymap::ab},
        {QStringLiteral("a"), keymap::aa},
        {QStringLiteral("equals"), keymap::equ},
        {QStringLiteral("="), keymap::equ},
        {QStringLiteral("multiply"), keymap::mult},
        {QStringLiteral("mult"), keymap::mult},
        {QStringLiteral("*"), keymap::mult},
        {QStringLiteral("power"), keymap::pow},
        {QStringLiteral("pow"), keymap::pow},

        // Row 5
        {QStringLiteral("var"), keymap::var},
        {QStringLiteral("minus"), keymap::minus},
        {QStringLiteral("-"), keymap::minus},
        {QStringLiteral("rightparen"), keymap::pright},
        {QStringLiteral(")"), keymap::pright},
        {QStringLiteral("pright"), keymap::pright},
        {QStringLiteral("decimal"), keymap::dot},
        {QStringLiteral("dot"), keymap::dot},
        {QStringLiteral("leftparen"), keymap::pleft},
        {QStringLiteral("("), keymap::pleft},
        {QStringLiteral("pleft"), keymap::pleft},
        {QStringLiteral("5"), keymap::n5},
        {QStringLiteral("catalog"), keymap::cat},
        {QStringLiteral("cat"), keymap::cat},
        {QStringLiteral("matrix"), keymap::metrix},
        {QStringLiteral("delete"), keymap::del},
        {QStringLiteral("del"), keymap::del},
        {QStringLiteral("backspace"), keymap::del},
        {QStringLiteral("pad"), keymap::pad},
        {QStringLiteral("scratchpad"), keymap::pad},

        // Row 6
        {QStringLiteral("flag"), keymap::flag},
        {QStringLiteral("plus"), keymap::plus},
        {QStringLiteral("+"), keymap::plus},
        {QStringLiteral("doc"), keymap::doc},
        {QStringLiteral("documents"), keymap::doc},
        {QStringLiteral("2"), keymap::n2},
        {QStringLiteral("menu"), keymap::menu},
        {QStringLiteral("8"), keymap::n8},
        {QStringLiteral("escape"), keymap::esc},
        {QStringLiteral("esc"), keymap::esc},
        {QStringLiteral("tab"), keymap::tab},

        // Row 7
        {QStringLiteral("shift"), keymap::shift},
        {QStringLiteral("ctrl"), keymap::ctrl},
        {QStringLiteral("control"), keymap::ctrl},
        {QStringLiteral("comma"), keymap::comma},
        {QStringLiteral(","), keymap::comma},
    };

    QString lower = keyName.toLower();
    if (keyNameMap.contains(lower)) {
        int key = keyNameMap[lower];
        return {key / keymap::COLS, key % keymap::COLS, true};
    }
    return {0, 0, false};
}

inline QStringList getAllKeyNames()
{
    return QStringList{
        QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
        QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8"), QStringLiteral("9"),
        QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("e"),
        QStringLiteral("f"), QStringLiteral("g"), QStringLiteral("h"), QStringLiteral("i"), QStringLiteral("j"),
        QStringLiteral("k"), QStringLiteral("l"), QStringLiteral("m"), QStringLiteral("n"), QStringLiteral("o"),
        QStringLiteral("p"), QStringLiteral("q"), QStringLiteral("r"), QStringLiteral("s"), QStringLiteral("t"),
        QStringLiteral("u"), QStringLiteral("v"), QStringLiteral("w"), QStringLiteral("x"), QStringLiteral("y"),
        QStringLiteral("z"),
        QStringLiteral("enter"), QStringLiteral("esc"), QStringLiteral("tab"), QStringLiteral("del"),
        QStringLiteral("space"), QStringLiteral("shift"), QStringLiteral("ctrl"),
        QStringLiteral("plus"), QStringLiteral("minus"), QStringLiteral("multiply"), QStringLiteral("divide"),
        QStringLiteral("equals"), QStringLiteral("power"),
        QStringLiteral("leftparen"), QStringLiteral("rightparen"), QStringLiteral("comma"), QStringLiteral("dot"),
        QStringLiteral("neg"),
        QStringLiteral("on"), QStringLiteral("menu"), QStringLiteral("doc"), QStringLiteral("var"),
        QStringLiteral("cat"), QStringLiteral("pad"), QStringLiteral("flag"),
        QStringLiteral("pi"), QStringLiteral("trig"), QStringLiteral("ee"), QStringLiteral("squ"),
        QStringLiteral("pow10")
    };
}

#endif // KEYMAP_MCP_H
