#pragma once

#include <Functions/FunctionFactory.h>
#include <Interpreters/InDepthNodeVisitor.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/IAST.h>

namespace DB
{


/// recursive traversal and check for optimizeGroupByFunctionKeys
struct KeepFunctionMatcher
{
    struct Data
    {
        std::unordered_set<String> & key_names_to_keep;
        bool & keep_key;
    };

    using Visitor = InDepthNodeVisitor<KeepFunctionMatcher, true>;

    static bool needChildVisit(const ASTPtr & node, const ASTPtr &)
    {
        return !(node->as<ASTFunction>());
    }

    static void visit(ASTFunction * function_node, Data & data)
    {
        if ((function_node->arguments->children).empty())
        {
            data.keep_key = true;
            return;
        }

        if (!data.key_names_to_keep.contains(function_node->getColumnName()))
        {
            Visitor(data).visit(function_node->arguments);
        }
    }

    static void visit(ASTIdentifier * ident, Data & data)
    {
        if (!data.key_names_to_keep.contains(ident->shortName()))
        {
            /// if variable of a function is not in GROUP BY keys, this function should not be deleted
            data.keep_key = true;
            return;
        }
    }

    static void visit(const ASTPtr & ast, Data & data)
    {
        if (data.keep_key)
            return;

        if (auto * function_node = ast->as<ASTFunction>())
        {
            visit(function_node, data);
        }
        else if (auto * ident = ast->as<ASTIdentifier>())
        {
            visit(ident, data);
        }
        else if (!ast->as<ASTExpressionList>())
        {
            data.keep_key = true;
        }
    }
};

using KeepFunctionVisitor = InDepthNodeVisitor<KeepFunctionMatcher, true>;

class GroupByFunctionKeysMatcher
{
public:
    struct Data
    {
        std::unordered_set<String> & key_names_to_keep;
    };

    static bool needChildVisit(const ASTPtr & node, const ASTPtr &)
    {
        return !(node->as<ASTFunction>());
    }

    static void visit(ASTFunction * function_node, Data & data)
    {
        bool keep_key = false;
        KeepFunctionVisitor::Data keep_data{data.key_names_to_keep, keep_key};
        KeepFunctionVisitor(keep_data).visit(function_node->arguments);

        if (!keep_key)
            (data.key_names_to_keep).erase(function_node->getColumnName());
    }

    static void visit(const ASTPtr & ast, Data & data)
    {
        if (auto * function_node = ast->as<ASTFunction>())
        {
            if (function_node->arguments && !function_node->arguments->children.empty())
                visit(function_node, data);
        }
    }
};

using GroupByFunctionKeysVisitor = InDepthNodeVisitor<GroupByFunctionKeysMatcher, true>;

}
