#pragma once

#include <DataTypes/getLeastSupertype.h>
#include <DataTypes/NumberTraits.h>
#include <Interpreters/castColumn.h>
#include <Columns/ColumnsNumber.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <base/map.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


enum class LeastGreatest : uint8_t
{
    Least,
    Greatest
};


template <LeastGreatest kind>
class FunctionLeastGreatestGeneric : public IFunction
{
public:
    static constexpr auto name = kind == LeastGreatest::Least ? "least" : "greatest";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionLeastGreatestGeneric<kind>>(); }

private:
    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isVariadic() const override { return true; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForNulls() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & types) const override
    {
        if (types.empty())
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} cannot be called without arguments", getName());

        return getLeastSupertype(types);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        if (arguments.size() == 1)
            return arguments[0].column;

        Columns converted_columns;
        for (const auto & argument : arguments)
        {
            if (argument.type->onlyNull())
                continue; /// ignore NULL arguments
            auto converted_col = castColumn(argument, result_type)->convertToFullColumnIfConst();
            converted_columns.emplace_back(converted_col);
        }

        if (converted_columns.empty())
            return arguments[0].column;
        else if (converted_columns.size() == 1)
            return converted_columns[0];

        auto result_column = result_type->createColumn();
        result_column->reserve(input_rows_count);

        for (size_t row_num = 0; row_num < input_rows_count; ++row_num)
        {
            size_t best_arg = 0;
            for (size_t arg = 1; arg < converted_columns.size(); ++arg)
            {
                if constexpr (kind == LeastGreatest::Least)
                {
                    auto cmp_result = converted_columns[arg]->compareAt(row_num, row_num, *converted_columns[best_arg], 1);
                    if (cmp_result < 0)
                        best_arg = arg;
                }
                else
                {
                    auto cmp_result = converted_columns[arg]->compareAt(row_num, row_num, *converted_columns[best_arg], -1);
                    if (cmp_result > 0)
                        best_arg = arg;
                }
            }

            result_column->insertFrom(*converted_columns[best_arg], row_num);
        }

        return result_column;
    }
};

template <LeastGreatest kind, typename SpecializedFunction>
class LeastGreatestOverloadResolver : public IFunctionOverloadResolver
{
public:
    static constexpr auto name = kind == LeastGreatest::Least ? "least" : "greatest";

    static FunctionOverloadResolverPtr create(ContextPtr context)
    {
        return std::make_unique<LeastGreatestOverloadResolver<kind, SpecializedFunction>>(context);
    }

    explicit LeastGreatestOverloadResolver(ContextPtr context_) : context(context_) {}

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isVariadic() const override { return true; }
    bool useDefaultImplementationForNulls() const override { return false; }

    FunctionBasePtr buildImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & return_type) const override
    {
        DataTypes argument_types;
        for (const auto & argument : arguments)
            argument_types.push_back(argument.type);

        /// More efficient specialization for two numeric arguments.
        if (arguments.size() == 2 && isNumber(arguments[0].type) && isNumber(arguments[1].type))
            return std::make_unique<FunctionToFunctionBaseAdaptor>(SpecializedFunction::create(context), argument_types, return_type);

        return std::make_unique<FunctionToFunctionBaseAdaptor>(
            FunctionLeastGreatestGeneric<kind>::create(context), argument_types, return_type);
    }

    DataTypePtr getReturnTypeImpl(const DataTypes & types) const override
    {
        if (types.empty())
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Function {} cannot be called without arguments", getName());

        if (types.size() == 2 && isNumber(types[0]) && isNumber(types[1]))
            return SpecializedFunction::create(context)->getReturnTypeImpl(types);

        return getLeastSupertype(types);
    }

private:
    ContextPtr context;
};

}
