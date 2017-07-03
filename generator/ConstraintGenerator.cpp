/******************************************************************************
 * Copyright (c) 2016 Leandro T. C. Melo (ltcmelo@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *****************************************************************************/

/*
 Contributors:
   - Marcus Rodrigues (demaroar@gmail.com)
*/

#include "ConstraintGenerator.h"
#include "ConstraintWriter.h"
#include "AST.h"
#include "Assert.h"
#include "BuiltinNames.h"
#include "Control.h"
#include "CoreTypes.h"
#include "Debug.h"
#include "Literals.h"
#include "Observer.h"
#include "PrintfScanner.h"
#include "Scope.h"
#include "Symbols.h"
#include "SymbolPP.h"
#include "TranslationUnit.h"
#include "TypeOfExpr.h"
#include "Utils.h"
#include <iostream>
#include <algorithm>

#define VISITOR_NAME "ConstraintGenerator"

#define OBSERVE(AST_NAME) ObserverInvoker<AST_NAME> invoker(observer_, ast, scope_)

#define ENSURE_NONEMPTY_TYPE_STACK(CODE) \
    PSYCHE_ASSERT(!types_.empty(), CODE, "type stack must be nonempty")

#define ENSURE_NONEMPTY_ALPHA_RET_STACK(CODE) \
    PSYCHE_ASSERT(!valuedRets_.empty(), CODE, "alpha return stack must be nonempty")

#define CLASSIFY(ast) \
    do { \
        if (preprocess_) { \
            lattice_.totalize(ast, scope_); \
        } \
    } while (false) \

using namespace CPlusPlus;
using namespace psyche;

namespace {

// Extract the identifier of a name. Use this only when it's guaranteed that
// the underlying name is indeed a simple one.
std::string extractId(const Name* name)
{
    PSYCHE_ASSERT(name && name->isNameId(),
                  return std::string(),
                  "expected simple name");

    const Identifier *id = name->asNameId()->identifier();
    return std::string(id->chars(), id->size());
}

}

std::string ConstraintGenerator::paramPrefix_ = "param";
std::string ConstraintGenerator::stubPrefix_ = "stub";

ConstraintGenerator::ConstraintGenerator(TranslationUnit *unit,
                                         ConstraintWriter* writer)
    : ASTVisitor(unit)
    , seenStmt_(false)
    , scope_(nullptr)
    , writer_(writer)
    , lattice_(unit)
    , staticDecl_(false)
    , preprocess_(true)
    , unnamedCount_(0)
    , observer_(nullptr)
{
    addVariadic("printf", 0);
    addVariadic("printf_s", 0);
    addVariadic("wprintf", 0);
    addVariadic("wprintf_s", 0);
    addVariadic("fprintf", 1);
    addVariadic("fprintf_s", 1);
    addVariadic("fwprintf", 1);
    addVariadic("fwprintf_s", 1);
    addVariadic("sprintf", 1);
    addVariadic("snprintf", 2);
    addVariadic("snprintf_s", 2);
    addVariadic("sprintf_s", 2);
    addVariadic("swprintf", 2);
    addVariadic("swprintf_s", 2);
    addVariadic("snwprintf_s", 2);
}

void ConstraintGenerator::prepareForRun()
{
    staticDecl_ = false;
    preprocess_ = false;
    supply_.resetCounter();
    unnamedCount_ = 0;
    knownFuncNames_.clear();
    knownFuncRets_.clear();

    static Observer dummy;
    if (!observer_)
        observer_ = &dummy;

    printDebug("Let's generate constraints!!!\n");
}

void ConstraintGenerator::generate(TranslationUnitAST *ast, Scope *global)
{
    if (!ast)
        return;

    global_ = global;

    // TODO: Split domain-lattice construction out of generator's implementation,
    // so we don't worry about "state" of constraint-specific data.
    auto writer = writer_;
    std::ostringstream unused;
    static ConstraintWriter dummy(unused);
    writer_ = &dummy;

    // Build the expression lattice.
    bool prevState = writer_->block(true);
    Scope *previousScope = switchScope(global);
    for (DeclarationListAST *it = ast->declaration_list; it; it = it->next)
        visitDeclaration(it->value);
    switchScope(previousScope);
    writer_->block(prevState);

    prepareForRun();
    writer_ = writer;

    // Constraint processing.
    previousScope = switchScope(global);
    OBSERVE(TranslationUnitAST);
    for (DeclarationListAST *it = ast->declaration_list; it; it = it->next) {
        visitDeclaration(it->value);
        if(it->next && ((it->value->asFunctionDefinition() &&
                        it->value->asFunctionDefinition()->function_body->asCompoundStatement()) ||
                        (it->value->asSimpleDeclaration() &&
                         it->value->asSimpleDeclaration()->symbols &&
                         it->value->asSimpleDeclaration()->symbols->value->storage() == Symbol::Typedef)
                       )) {
            writer_->clearIndent();
            writer_->writeAnd(true);
        }
    }
    switchScope(previousScope);
}

void ConstraintGenerator::installObserver(Observer *observer)
{
    PSYCHE_ASSERT(observer, return, "expected valid observer");

    observer_ = observer;
    observer_->setConstraintWriter(writer_);
}

void ConstraintGenerator::addVariadic(const std::string &funcName, size_t varArgPos)
{
    variadic_.insert(std::make_pair(funcName, varArgPos));
}

Scope *ConstraintGenerator::switchScope(Scope *scope)
{
    if (!scope)
        return scope_;
    std::swap(scope_, scope);
    return scope;
}

void ConstraintGenerator::pushType(const std::string &ty)
{
    types_.push(ty);
}

std::string ConstraintGenerator::popType()
{
    std::string ty = types_.top();
    types_.pop();
    return ty;
}

std::string ConstraintGenerator::createUnnamed(const std::string& prefix)
{
    int count = ++unnamedCount_;
    return prefix + std::to_string(count);
}

void ConstraintGenerator::assignTop(const std::string& name)
{
    writer_->writeTypeof(name);
    writer_->writeEquivMark();
    ENSURE_NONEMPTY_TYPE_STACK(return);
    writer_->writeTypeName(types_.top());
}

void ConstraintGenerator::collectExpression(const std::string &ty,
                                            ExpressionAST *expr)
{
    pushType(ty);
    visitExpression(expr);
    ENSURE_NONEMPTY_TYPE_STACK(return);
    popType();
}

    //--- Declarations

void ConstraintGenerator::visitDeclaration(DeclarationAST *ast)
{
    accept(ast);
}

bool ConstraintGenerator::visit(FunctionDefinitionAST *ast)
{
    DEBUG_VISIT(FunctionDefinitionAST);
    OBSERVE(FunctionDefinitionAST);

    Function* func = ast->symbol;
    if (!func->name()->isNameId())
        return false;

    visitSymbol(func, ast->function_body);

    // Time to write any pending equivalences.
    auto writePending = [this]() {
        const EquivPair& equiv = pendingEquivs_.top();
        writer_->writeEquivRel(equiv.first, equiv.second);
        pendingEquivs_.pop();
    };
    while (!pendingEquivs_.empty()) {
        maybeFollowStmt();
        writePending();
    }

    writer_->clearIndent();
    writer_->writeLineBreak();
    seenStmt_ = false;

    return false;
}

void ConstraintGenerator::visitSymbol(Function *func, StatementAST* body)
{
    std::vector<ConstraintWriter::ParamPair> params;
    if (func->hasArguments()) {
        for (auto i = 0u; i < func->argumentCount(); i++) {
            Symbol *arg = func->argumentAt(i);
            std::string specifier = typeSpeller_.spell(arg->type(), scope_);

            const Name *argName = arg->name();
            if(argName) {
                const Identifier *idArgName = argName->asNameId()->identifier();
                std::string declarator(idArgName->begin(), idArgName->end());
                params.emplace_back(specifier, declarator);
            } else {
                params.emplace_back(specifier, createUnnamed(paramPrefix_));
            }
        }

        // Write their types.
        std::vector<ConstraintWriter::ParamPair> paramsTypes;
        std::vector<ConstraintWriter::ParamPair>::const_iterator it;
        for(it = params.begin(); it != params.end(); ++it) {
            std::string alpha = supply_.createTypeVar1();
            paramsTypes.emplace_back(alpha, (*it).first);
            writer_->writeExists(alpha);
        }
        for(it = paramsTypes.begin(); it != paramsTypes.end(); ++it) {
            writer_->writeTypedef((*it).second, (*it).first);
            writer_->writeAnd(true);
        }
    }

    // Write the function prototype. If no return type is specified, we adopt old-style C
    // rule and assume it's `int'.
    std::string funcRet;
    if (func->returnType())
        funcRet = typeSpeller_.spell(func->returnType(), scope_);
    else
        funcRet = kDefaultIntTy;

    const std::string& alpha = ensureTypeIsKnown(funcRet);
    const Identifier *id = func->name()->asNameId()->identifier();
    const std::string funcName(id->begin(), id->end());
    writer_->writeFuncDecl(funcName, params, funcRet);

    if (func->storage() == Symbol::Static) {
        writer_->writeStatic(funcName);
        writer_->writeAnd(true);
    }

    // Flag that we have no return value information about this function.
    valuedRets_.push(false);

    // Enter the function's body, if a definition.
    if (body) {
        pushType(funcRet);
        Scope *previousScope = switchScope(func->asScope());
        visitStatement(body);
        switchScope(previousScope);
        popType();
    }

    // If no valued return was detected and its return is of an unknown type, we add an
    // equivalence between it and its alpha.
    ENSURE_NONEMPTY_ALPHA_RET_STACK(return);
    bool hasValue = valuedRets_.top();
    valuedRets_.pop();
    if (!hasValue && !alpha.empty())
        pendingEquivs_.push(EquivPair(alpha, funcRet));

    // Keep track of function return types, since we can use this information for function
    // calls that are expression statements.
    knownFuncNames_.insert(std::make_pair(funcName, funcRet));
    auto it = knownFuncRets_.find(funcName);
    if (it != knownFuncRets_.end()) {
        for (const auto& ignored : it->second)
            pendingEquivs_.push(EquivPair(ignored, funcRet));
    }
}

bool ConstraintGenerator::visit(SimpleDeclarationAST *ast)
{
    DEBUG_VISIT(SimpleDeclarationAST);
    OBSERVE(SimpleDeclarationAST);

    for (SpecifierListAST *it = ast->decl_specifier_list; it; it = it->next)
        visitSpecifier(it->value);

    if (!ast->declarator_list)
        return false;

    // For each symbol annotated in the AST, there must exist a corresponding
    // declarator, and we can uniformily iterate over the two lists.
    DeclaratorListAST *declIt = ast->declarator_list;
    for (const List<Symbol*> *symIt = ast->symbols; symIt; symIt = symIt->next) {
        PSYCHE_ASSERT(declIt->value, return false, "expected declarator");

        // Current symbol.
        Symbol *decl = symIt->value;
        const Name *name = decl->name();
        std::string declName = extractId(name);
        std::string declTy = typeSpeller_.spell(decl->type(), scope_);

        // Type a function declaration just as we do for a function definition.
        if (decl->asDeclaration()
                && decl->asDeclaration()->type()
                && decl->asDeclaration()->type()->asFunctionType()) {
            visitSymbol(decl->asDeclaration()->type()->asFunctionType()->asFunction(), nullptr);
            return false;
        }

        // Altough a `typedef` is parsed as a simple declaration, its contraint
        // rule is different. We process it and break out, since there cannot
        // exist multiple typedefs within one declaration.
        if (decl->storage() == Symbol::Typedef) {
            writer_->writeTypedef(declName, declTy);
            writer_->writeLineBreak();
            PSYCHE_ASSERT(!symIt->next, return false,
                          "multiple symbols within typedef cannot exist");
            return false;
        }

        const std::string& alpha = ensureTypeIsKnown(declTy);

        // If an initializer is provided, visit the expression. Unless, it
        // contains braces. If the declaration is of a record, we'll only
        // generate constraints if one of its fields are accessed. If the
        // declaration if of an array, we'll deal with it further down.
        if (declIt->value->initializer
                && !declIt->value->initializer->asBracedInitializer()) {
            const std::string& rhsAlpha = supply_.createTypeVar1();
            writer_->writeExists(rhsAlpha);
            pushType(rhsAlpha);
            visitExpression(declIt->value->initializer);
            writer_->writeAnd(true);

            applyTypeLattice(lattice_.recover(decl), classOfExpr(declIt->value->initializer),
                             alpha, rhsAlpha, T_EQUAL);
            popType();
            writer_->writeAnd(true);
        }

        writer_->writeVarDecl(declName, alpha);

        // If this is the declaration of a struct member, we need to generate
        // a containment relationship for it.
        if (!structs_.empty()) {
            const std::string& structTy = structs_.top();
            writer_->writeMemberRel(structTy, declName, alpha);
            writer_->writeAnd(true);
        }

        // When we have an array with an initializer, we use the individual
        // expressions within the brace to determine the underlying element
        // type of the array. There's an apparent limitation to this approach
        // because C allows one to omit "internal" braces of composite types.
        // A `typedef struct Pair_ { int x; int y} Pair;' can be initialized
        // with any of the following forms:
        //     Pair array1[] = { { 1, 2 }, { 3, 4 } };
        //     Pair array1[] = {   1, 2  ,   3, 4   };
        // Looking at this syntax only we cannot distinguish between a pair
        // type array with two elements or a int type array. We handle this
        // through an pre-generation lattice classificaiton, just like we do
        // for the pointer x integrals conversions issue.
        // The first expression to process in an array declarator is its size
        // specification withing the brakets.
        if (declIt->value->postfix_declarator_list
                && declIt->value->postfix_declarator_list->value->asArrayDeclarator()) {
            auto decltr = declIt->value->postfix_declarator_list;
            do {
                auto size = decltr->value->asArrayDeclarator()->expression;
                if (size) {
                    collectExpression(kDefaultIntTy, size);
                    writer_->writeAnd();
                }
                decltr = decltr->next;
            } while (decltr && decltr->value->asArrayDeclarator());

            if (declIt->value->initializer
                    && declIt->value->initializer->asBracedInitializer()
                    && declIt->value->initializer->asBracedInitializer()->expression_list) {
                std::string elem = supply_.createTypeVar1();
                writer_->writeExists(elem);

                auto init = declIt->value->initializer->asBracedInitializer()->expression_list;
                if (scope_ == global_ || decl->storage() == Symbol::Static)
                    staticDecl_ = true;
                do {
                    collectExpression(elem, init->value);
                    init = init->next;
                    if (!init)
                        break;
                    writer_->writeAnd();
                } while (true);
                staticDecl_ = false; // Just make it false, cannot nest.

                auto decltrs = declIt->value->postfix_declarator_list;
                do {
                    writer_->writeAnd();
                    const std::string& ptr = supply_.createTypeVar1();
                    writer_->writeExists(ptr);
                    writer_->writePtrRel(ptr, elem);
                    elem = ptr;
                    decltrs = decltrs->next;
                } while (decltrs && decltrs->value->asArrayDeclarator());
                writer_->writeAnd();
                writer_->writeEquivRel(alpha, elem);
                writer_->writeAnd(true);
            }
        }

        declIt = declIt->next;

        // Remember about the pending type equivalence for this declaration,
        // which we write down only after traversing the remaining statements.
        pendingEquivs_.push(EquivPair(alpha, declTy));
    }

    return false;
}

    //--- Names

void ConstraintGenerator::visitName(NameAST *ast)
{
    accept(ast);
}

std::string ConstraintGenerator::ensureTypeIsKnown(std::string& tyName)
{
    std::string alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    writer_->writeTypedef(tyName, alpha);
    writer_->writeAnd(true);
    return alpha;
}

    //--- Expressions

void ConstraintGenerator::visitExpression(ExpressionAST *ast)
{
    accept(ast);
}

namespace {

const std::string nonPolyOprtrName(int opTk)
{
    switch (opTk) {
    case T_STAR:
        return "*";

    case T_SLASH:
        return "/";

    case T_PERCENT:
        return "%";

    case T_AMPER:
        return "&";

    case T_PIPE:
        return "|";

    case T_CARET:
        return "^";

    case T_LESS_LESS:
        return "<<";

    case T_GREATER_GREATER:
        return ">>";

    default:
        PSYCHE_ASSERT(false, return "", "operator is polymorphic");
        return "";
    }
}

/*!
 * \brief isPolyOprtr
 * \param opTk
 *
 * Return whether the operator is polymorhpic. But we don't consider polymorphism
 * among numeric types themsleves (i.e. between integers and floating points).
 * We are interested on operators in which pointer types can be operands.
 */
bool isPolyOprtr(int opTk)
{
    switch (opTk) {
        // Assignment operators
    case T_EQUAL:
    case T_PLUS_EQUAL:
    case T_MINUS_EQUAL:
    case T_STAR_EQUAL:
    case T_SLASH_EQUAL:
    case T_PERCENT_EQUAL:
    case T_LESS_LESS_EQUAL:
    case T_GREATER_GREATER_EQUAL:
    case T_CARET_EQUAL:
    case T_AMPER_EQUAL:
    case T_PIPE_EQUAL:
        return true;

        // Additive operators
    case T_PLUS:
    case T_MINUS:
        return true;

        // Multiplicative operators
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return true;

        // Bitwise operators
    case T_AMPER:
    case T_PIPE:
    case T_CARET:
        return true;

        // Shift operators
    case T_LESS_LESS:
    case T_GREATER_GREATER:
        return false;

        // Logical operators
    case T_AMPER_AMPER:
    case T_PIPE_PIPE:
        return true;

        // Equality operators
    case T_EXCLAIM_EQUAL:
    case T_EQUAL_EQUAL:
        return true;

        // Relational operators.
    case T_LESS:
    case T_GREATER:
    case T_LESS_EQUAL:
    case T_GREATER_EQUAL:
        return true;

    default:
        PSYCHE_ASSERT(false, return false, "unhandled binary operator");
        break;
    }
}

/*!
 * \brief isPolyOprtrArithRet
 * \param opTk
 *
 * Return whether the return type of an expression involving the given operator
 * is of an arithmetic type.
 */
bool isPolyOprtrArithRet(int opTk)
{
    switch (opTk) {
    case T_PLUS:
    case T_MINUS:
    case T_AMPER_AMPER:
    case T_PIPE_PIPE:
    case T_EXCLAIM_EQUAL:
    case T_EQUAL_EQUAL:
    case T_LESS:
    case T_GREATER:
    case T_LESS_EQUAL:
    case T_GREATER_EQUAL:
        return true;

    default:
        return false;
    }
}

} // anonymous

void ConstraintGenerator::applyTypeLattice(const DomainLattice::Class& lhsClass,
                                           const DomainLattice::Class& rhsClass,
                                           const std::string& lhsAlpha,
                                           const std::string& rhsAlpha,
                                           int opTk)
{
    // Now we decide whether we generate or discard the constraints for this
    // binary expression. Whenever one of the operands is a pointer and the
    // other is an arithmetic type (integral or floating point), the constraint
    // is dropped so we don't trigger an "overunification".

    if ((lhsClass == DomainLattice::Pointer
         && (rhsClass == DomainLattice::Integral
             || rhsClass == DomainLattice::FloatingPoint
             || rhsClass == DomainLattice::Arithmetic
             || rhsClass == DomainLattice::Scalar))
            || ((lhsClass == DomainLattice::Integral
                 || lhsClass == DomainLattice::FloatingPoint
                 || lhsClass == DomainLattice::Arithmetic
                 || lhsClass == DomainLattice::Scalar)
                && (rhsClass == DomainLattice::Pointer))) {
        printDebug("Discard constraint, %s x %s\n",
                   lhsClass.name_.c_str(), rhsClass.name_.c_str());
        writer_->writeTruth();
        return;
    }

    printDebug("No constraint tweak, %s x %s\n",
               lhsClass.name_.c_str(), rhsClass.name_.c_str());

    // At this point, we know there's no conflict between pointers and
    // arithmetic types. If either one of the sides is of an arithmetic
    // type, then both are, we only need to pick a double or an int.
    std::string actualArithTy = (lhsClass > rhsClass) ? lhsClass.arithName_
                                                      : rhsClass.arithName_;

    if (actualArithTy.empty()) {
        if (lhsClass == DomainLattice::Integral
                || rhsClass == DomainLattice::Integral
                || lhsClass == DomainLattice::Arithmetic
                || rhsClass == DomainLattice::Arithmetic) {
            actualArithTy = kDefaultIntTy;
        } else if (lhsClass == DomainLattice::FloatingPoint
                   || rhsClass == DomainLattice::FloatingPoint) {
            actualArithTy = kDefaultFloatPointTy;
        }
    }

    // We must be careful with polymorphic operators such as `+' or `-' since
    // they work on both pointers and integers. A constraint for a concrete
    // type may only be generated in the case both operands are compatible.
    if (isPolyOprtr(opTk)) {
        if (opTk == T_EQUAL)
            writer_->writeSubtypeRel(lhsAlpha, rhsAlpha);
        else
            writer_->writeEquivRel(lhsAlpha, rhsAlpha);

        if (!actualArithTy.empty()) {
            writer_->writeAnd();
            writer_->writeEquivRel(lhsAlpha, actualArithTy);
        }
        if (isPolyOprtrArithRet(opTk)) {
            writer_->writeAnd();
            writer_->writeEquivRel(types_.top(),
                                        actualArithTy.empty() ? kDefaultArithTy : actualArithTy);
        }
        return;
    }

    writer_->writeTruth();
}

DomainLattice::Class ConstraintGenerator::classOfExpr(ExpressionAST *ast) const
{
    const auto& s = lattice_.fetchText(ast);
    auto clazz = lattice_.recover(ast);
    if (clazz != DomainLattice::Undefined) {
        printDebug("Recovered AST %s as %s\n", s.c_str(), clazz.name_.c_str());
    } else {
        TypeOfExpr typeofExpr(translationUnit());
        FullySpecifiedType ty = typeofExpr.resolve(ast, scope_);
        clazz = DomainLattice::classOf(ty);
        printDebug("Typed AST %s as %s\n", s.c_str(), clazz.name_.c_str());
    }

    return clazz;
}


bool ConstraintGenerator::visit(ArrayAccessAST *ast)
{
    DEBUG_VISIT(ArrayAccessAST);
    OBSERVE(ArrayAccessAST);
    CLASSIFY(ast);

    std::tuple<std::string, std::string, std::string> a1a2a3 = supply_.createTypeVar3();
    writer_->writeExists(std::get<0>(a1a2a3));
    writer_->writeExists(std::get<1>(a1a2a3));
    writer_->writeExists(std::get<2>(a1a2a3));
    collectExpression(std::get<0>(a1a2a3), ast->base_expression);

    writer_->writeAnd();
    collectExpression(std::get<1>(a1a2a3), ast->expression);

    writer_->writeAnd();
    writer_->writePtrRel(std::get<0>(a1a2a3), std::get<2>(a1a2a3));

    writer_->writeAnd();
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeEquivRel(types_.top(), std::get<2>(a1a2a3));

    writer_->writeAnd();
    writer_->writeEquivRel(std::get<1>(a1a2a3), kDefaultIntTy);

    return false;
}

bool ConstraintGenerator::visit(BinaryExpressionAST *ast)
{
    DEBUG_VISIT(BinaryExpressionAST);
    OBSERVE(BinaryExpressionAST);
    CLASSIFY(ast);

    // A comma expression is irrelevant for us. We consider it merely as a
    // sequence of unrelated expressions.
    unsigned opTk = tokenKind(ast->binary_op_token);
    if (opTk == T_COMMA) {
        visitExpression(ast->left_expression);
        if (ast->right_expression) {
            writer_->writeAnd(true);
            visitExpression(ast->right_expression);
        }
        return false;
    }

    // We don't know the underlying element type of the pointer, but we can
    // at least constrain the left-hand-side as a pointer.
    ExpressionAST* left = stripParen(ast->left_expression);
    ExpressionAST* right = stripParen(ast->right_expression);
    if (right->asPointerLiteral()) {
        std::tuple<std::string, std::string> a1a2 = supply_.createTypeVar2();
        writer_->writeExists(std::get<0>(a1a2));
        writer_->writeExists(std::get<1>(a1a2));
        collectExpression(std::get<0>(a1a2), left);
        writer_->writeAnd();
        writer_->writePtrRel(std::get<0>(a1a2), std::get<1>(a1a2));
        return false;
    }

    std::tuple<std::string, std::string> a1a2 = supply_.createTypeVar2();
    writer_->writeExists(std::get<0>(a1a2));
    writer_->writeExists(std::get<1>(a1a2));
    collectExpression(std::get<0>(a1a2), left);
    writer_->writeAnd();
    collectExpression(std::get<1>(a1a2), right);
    writer_->writeAnd();

    applyTypeLattice(classOfExpr(left), classOfExpr(right),
                     std::get<0>(a1a2), std::get<1>(a1a2),
                     opTk);

    if (!isPolyOprtr(opTk)) {
        writer_->writeAnd();
        writer_->writeTypeof(nonPolyOprtrName(opTk));
        writer_->writeEquivMark();
        writer_->enterGroup();
        ENSURE_NONEMPTY_TYPE_STACK(return false);
        writer_->writeTypeNames(
        { std::get<0>(a1a2), std::get<1>(a1a2), types_.top() });
        writer_->leaveGroup();
    }

    return false;
}

namespace {

const std::string trivialName(IdExpressionAST* idExpr)
{
    PSYCHE_ASSERT(idExpr
                  && idExpr->name->name->asNameId()
                  && idExpr->name->name->asNameId()->identifier(),
                  return std::string(),
                  "expected a trivial name");

    const Identifier *id = idExpr->name->name->asNameId()->identifier();
    return std::string(id->chars(), id->size());
}

} // anonymous

bool ConstraintGenerator::visit(CallAST *ast)
{
    DEBUG_VISIT(CallAST);
    OBSERVE(CallAST);
    CLASSIFY(ast);

    std::string funcName;
    int varArgPos = -1;
    if (ast->base_expression->asIdExpression()) {
        funcName = trivialName(ast->base_expression->asIdExpression());

        // Detect whether the function is registered as a variadic one.
        auto fit = variadic_.find(funcName);
        if (fit != variadic_.end())
            varArgPos = fit->second;
    } else {
        const std::string& funcVar = supply_.createTypeVar1();
        writer_->writeExists(funcVar);
        collectExpression(funcVar, ast->base_expression);

        funcName = createUnnamed(stubPrefix_);
        writer_->writeAnd();
        writer_->writeTypeof(funcName);
        writer_->writeEquivMark();
        writer_->writeTypeName(funcVar);
        writer_->writeAnd();
    }

    // Deal with "regular" functions, for which we generate constraints through
    // the normal expression inspection process.
    if (varArgPos == -1) {
        std::vector<std::string> typeVars;
        for (ExpressionListAST* it = ast->expression_list; it; it = it->next) {
            const std::string& typeVar = supply_.createTypeVar1();
            writer_->writeExists(typeVar);
            collectExpression(typeVar, it->value);
            typeVars.push_back(typeVar);
            if (it->next)
                writer_->writeAnd(true);
        }
        ENSURE_NONEMPTY_TYPE_STACK(return false);
        typeVars.push_back(types_.top());


        if (ast->expression_list)
            writer_->writeAnd();
        writer_->writeTypeof(funcName);
        writer_->writeEquivMark();
        writer_->enterGroup();
        writer_->writeTypeNames(typeVars);
        writer_->leaveGroup();

        return false;
    }

    // Deal with printf family of functions, for which we generate constraints
    // based on format specifiers.
    int argCnt = 0;
    std::vector<PrintfScanner::FormatSpec> specs;
    for (ExpressionListAST* it = ast->expression_list; it; it = it->next, ++argCnt) {
        if (argCnt < varArgPos) {
            const std::string& typeVar = supply_.createTypeVar1();
            writer_->writeExists(typeVar);
            collectExpression(typeVar, it->value);
            if (it->next)
                writer_->writeAnd(true);
            continue;
        }

        if (argCnt == varArgPos) {
            // We expect the formatting string as a literal expression. But in
            // order to be more robust we also accept a literal wrapped in
            // parenthesis or within a call expression in which the base is an
            // identifier expression. The reason for this approach is because
            // many projects definte "translation macros" for string literals.
            // But those function-like macros are possibly unexpanded in the
            // code we analyse, so our parser will parse them as regular functions.
            ExpressionAST* litExpr = it->value;
            while (litExpr->asNestedExpression())
                litExpr = litExpr->asNestedExpression()->expression;
            if (litExpr->asCall() && litExpr->asCall()->expression_list)
                litExpr = litExpr->asCall()->expression_list->value;
            if (litExpr->asStringLiteral()) {
                specs.resize(argCnt);
                specs.push_back(PrintfScanner::String);

                do {
                    const StringLiteral* lit =
                        stringLiteral(litExpr->asStringLiteral()->literal_token);
                    std::string format(lit->begin(), lit->end());
                    PrintfScanner scanner(format);
                    // Although we already know the format is a string, we still
                    // add it to the evaluation for the case in which there are
                    // no type specifiers in it - we need a constraint out of this
                    // due to workflow reasons.
                    for (auto spec : scanner.evaluate())
                        specs.push_back(spec);

                    // Our lexer doesn't concatenate adjacent string literal
                    // tokens, they are instead stored in the literal itself.
                    litExpr = litExpr->asStringLiteral()->next;
                } while (litExpr);
            } else {
                // When the format is not a string literal, there's not much
                // we can do, except an string equivalence on the expression.
                collectExpression(kDefaultStrTy, it->value);
                return false;
            }
        }

        if (argCnt >= varArgPos) {
            PSYCHE_ASSERT(it->value && argCnt < static_cast<int>(specs.size()), return false,
                          "expected matching number of exprs/specs");
            switch (specs[argCnt]) {
            case PrintfScanner::Char:
            case PrintfScanner::Int:
                collectExpression(kDefaultIntTy, it->value);
                break;

            case PrintfScanner::FloatingPoint:
                collectExpression(kDefaultFloatPointTy, it->value);
                break;

            case PrintfScanner::String:
                collectExpression(kDefaultStrTy, it->value);
                break;

            case PrintfScanner::Pointer:
                // We don't know the underlying pointer type, nothing interesting to do.
                writer_->writeTruth();
                break;

            default:
                PSYCHE_ASSERT(false, return false, "unknown type spec");
                break;
            }
            if (it->next)
                writer_->writeAnd();
        }
    }

    return false;
}


void ConstraintGenerator::castExpressionHelper(const std::string& inputTy,
                                               std::string& resultTy)
{
    if (resultTy != kCharTy
            && resultTy != kShortTy
            && resultTy != kIntTy
            && resultTy != kLongTy
            && resultTy != kLongLongTy
            && resultTy != kFloatTy
            && resultTy != kDoubleTy
            && resultTy != kLongDouble
            && resultTy != kVoidTy
            && resultTy != kDefaultStrTy) {
        ensureTypeIsKnown(resultTy);
    }
    ENSURE_NONEMPTY_TYPE_STACK(return );
    writer_->writeEquivRel(inputTy, resultTy);
}

bool ConstraintGenerator::visit(CastExpressionAST *ast)
{
    DEBUG_VISIT(CastExpressionAST);
    OBSERVE(CastExpressionAST);
    CLASSIFY(ast);

    std::string ty = typeSpeller_.spell(ast->expression_type, scope_);
    castExpressionHelper(types_.top(), ty);

    writer_->writeAnd();
    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    collectExpression(alpha, ast->expression);

    return false;
}

void ConstraintGenerator::convertBoolExpression(ExpressionAST *ast)
{
    std::string ty;
    auto clazz = classOfExpr(ast);
    if (clazz == DomainLattice::Arithmetic) {
        ty = kDefaultArithTy;
    } else if (clazz == DomainLattice::Integral) {
        ty = kDefaultIntTy;
    } else if (clazz == DomainLattice::FloatingPoint) {
        ty = kDefaultFloatPointTy;
    } else {
        ty = supply_.createTypeVar1();
        writer_->writeExists(ty);
    }

    collectExpression(ty, ast);
}

bool ConstraintGenerator::visit(ConditionalExpressionAST *ast)
{
    DEBUG_VISIT(ConditionalExpressionAST);
    OBSERVE(ConditionalExpressionAST);
    CLASSIFY(ast);

    convertBoolExpression(ast->condition);

    writer_->writeAnd();
    visitExpression(ast->left_expression);
    writer_->writeAnd();
    visitExpression(ast->right_expression);

    return false;
}

bool ConstraintGenerator::visit(IdExpressionAST *ast)
{
    DEBUG_VISIT(IdExpressionAST);
    OBSERVE(IdExpressionAST);
    CLASSIFY(ast);

    assignTop(extractId(ast->name->name));

    if (staticDecl_) {
        // Static initialization requires a compile-time constant.
        writer_->writeAnd();
        writer_->writeReadOnly(extractId(ast->name->name));
    }

    return false;
}

bool ConstraintGenerator::visit(NumericLiteralAST *ast)
{
    DEBUG_VISIT(NumericLiteralAST);
    OBSERVE(NumericLiteralAST);
    CLASSIFY(ast);

    const NumericLiteral *numLit = numericLiteral(ast->literal_token);
    PSYCHE_ASSERT(numLit, return false, "numeric literal must exist");
    if (numLit->isDouble()
            || numLit->isFloat()
            || numLit->isLongDouble()) {
        writer_->writeTypeName(kDefaultFloatPointTy);
    } else {
        if (!strcmp(numLit->chars(), "0")) {
            ENSURE_NONEMPTY_TYPE_STACK(return false);
            writer_->writeTypeName(types_.top());
        } else if (tokenKind(ast->literal_token) == T_CHAR_LITERAL) {
            writer_->writeTypeName(kCharTy);
        } else {
            writer_->writeTypeName(kDefaultIntTy);
        }
    }
    writer_->writeEquivMark();
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeTypeName(types_.top());

    return false;
}

bool ConstraintGenerator::visit(BoolLiteralAST *ast)
{
    DEBUG_VISIT(BoolLiteralAST);
    OBSERVE(BoolLiteralAST);
    CLASSIFY(ast);

    // Treated as integer. It's relatively common to have C code defining `true`
    // and `false` through macros. Their meaning is obvious in such cases.
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeEquivRel(kDefaultIntTy, types_.top());

    return false;
}

bool ConstraintGenerator::visit(StringLiteralAST *ast)
{
    DEBUG_VISIT(StringLiteralAST);
    OBSERVE(StringLiteralAST);
    CLASSIFY(ast);

    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeEquivRel(kDefaultStrTy, types_.top());

    return false;
}

bool ConstraintGenerator::visit(MemberAccessAST *ast)
{
    DEBUG_VISIT(MemberAccessAST);
    OBSERVE(MemberAccessAST);
    CLASSIFY(ast);

    std::tuple<std::string, std::string> a1a2 = supply_.createTypeVar2();
    writer_->writeExists(std::get<0>(a1a2));
    writer_->writeExists(std::get<1>(a1a2));

    std::string alpha3;
    unsigned accessTk = tokenKind(ast->access_token);
    if (accessTk == T_ARROW)  {
        alpha3 = supply_.createTypeVar1();
        writer_->writeExists(alpha3);
    }

    collectExpression(std::get<0>(a1a2), ast->base_expression);

    // For a pointer access we need to insert an additional constraint.
    if (accessTk == T_ARROW) {
        writer_->writeAnd();
        writer_->writePtrRel(std::get<0>(a1a2), std::get<1>(a1a2));
    }

    std::string sym = extractId(ast->member_name->name);
    writer_->writeAnd();
    if (accessTk == T_ARROW)
        writer_->writeMemberRel(std::get<1>(a1a2), sym, alpha3);
    else
        writer_->writeMemberRel(std::get<0>(a1a2), sym, std::get<1>(a1a2));

    writer_->writeAnd();
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    if (accessTk == T_ARROW)
        writer_->writeEquivRel(types_.top(), alpha3);
    else
        writer_->writeEquivRel(types_.top(), std::get<1>(a1a2));

    return false;
}

bool ConstraintGenerator::visit(BracedInitializerAST *ast)
{
    DEBUG_VISIT(BracedInitializerAST);
    OBSERVE(BracedInitializerAST);
    CLASSIFY(ast);

    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);

    auto cnt = 0;
    for (auto it = ast->expression_list; it; it = it->next, ++cnt) {
        const std::string& member = supply_.createTypeVar1();
        writer_->writeExists(member);
        collectExpression(member, it->value);
        writer_->writeAnd();
        // TODO: We need a member name placeholder!
        writer_->writeMemberRel(alpha, "member_" + std::to_string(cnt), member);
        if (it->next)
            writer_->writeAnd();
    }

    writer_->writeAnd();
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeEquivRel(types_.top(), alpha);

    return false;
}

bool ConstraintGenerator::visit(PostIncrDecrAST *ast)
{
    DEBUG_VISIT(PostIncrDecrAST);
    OBSERVE(PostIncrDecrAST);
    CLASSIFY(ast);

    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    collectExpression(alpha, ast->base_expression);

    return false;
}

bool ConstraintGenerator::visit(UnaryExpressionAST* ast)
{
    DEBUG_VISIT(UnaryExpressionAST);
    OBSERVE(UnaryExpressionAST);
    CLASSIFY(ast);

    switch(tokenKind(ast->unary_op_token)) {
    case T_AMPER: {
        std::tuple<std::string, std::string> a1a2 = supply_.createTypeVar2();
        writer_->writeExists(std::get<0>(a1a2));
        writer_->writeExists(std::get<1>(a1a2));
        collectExpression(std::get<1>(a1a2), ast->expression);

        writer_->writeAnd();
        writer_->writePtrRel(std::get<0>(a1a2), std::get<1>(a1a2));

        writer_->writeAnd();
        ENSURE_NONEMPTY_TYPE_STACK(return false);
        writer_->writeEquivRel(types_.top(), std::get<0>(a1a2));
        break;
    }

    case T_STAR: {
        const std::string& alpha = supply_.createTypeVar1();
        writer_->writeExists(alpha);
        collectExpression(alpha, ast->expression);
        writer_->writeAnd();
        ENSURE_NONEMPTY_TYPE_STACK(return false);
        writer_->writePtrRel(alpha, types_.top());
        break;
    }

    case T_EXCLAIM: {
        const std::string& alpha = supply_.createTypeVar1();
        writer_->writeExists(alpha);
        collectExpression(alpha, ast->expression);
        break;
    }

    default:
        // Let's visit the base expression.
        visitExpression(ast->expression);
        break;
    }

    return false;
}

bool ConstraintGenerator::visit(SizeofExpressionAST *ast)
{
    DEBUG_VISIT(SizeofExpressionAST);
    OBSERVE(SizeofExpressionAST);
    CLASSIFY(ast);

    // When sizeof's argument is a type, we need to make sure it exists.
    if (ast->expression->asTypeId()) {
        const std::string& alpha = supply_.createTypeVar1();
        writer_->writeExists(alpha);
        const std::string& ty = typeSpeller_.spell(ast->expression_type, scope_);
        writer_->writeTypedef(ty, alpha);
        writer_->writeAnd(true);
    }

    // TODO: Make sizeof and related type as size_t.
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writeEquivRel(types_.top(), kDefaultIntTy);

    return false;
}

bool ConstraintGenerator::visit(CPlusPlus::PointerLiteralAST* ast)
{
    DEBUG_VISIT(PointerLiteralAST);
    CLASSIFY(ast);

    // We don't know the underlying element type, but we know it's a pointer.
    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    ENSURE_NONEMPTY_TYPE_STACK(return false);
    writer_->writePtrRel(types_.top(), alpha);

    return false;
}

    //--- Specifiers

void ConstraintGenerator::visitSpecifier(SpecifierAST *ast)
{
    accept(ast);
}

bool ConstraintGenerator::visit(EnumSpecifierAST *ast)
{
    DEBUG_VISIT(EnumSpecifierAST);
    OBSERVE(EnumSpecifierAST);

    for (SpecifierListAST *it = ast->type_specifier_list; it; it = it->next)
        visitSpecifier(it->value);

    Scope *previousScope = switchScope(ast->symbol);
    for (EnumeratorListAST *it = ast->enumerator_list; it; it = it->next)
        visitExpression(it->value->expression);
    switchScope(previousScope);

    return false;
}

bool ConstraintGenerator::visit(ClassSpecifierAST* ast)
{
    DEBUG_VISIT(ClassSpecifierAST);
    OBSERVE(ClassSpecifierAST);

    std::string classTy = typeSpeller_.spell(ast->symbol->type(), scope_);
    std::string tyName;
    if (ast->name->name->asAnonymousNameId()) {
        tyName = classTy;
    } else {
        std::string head = "struct ";
        if (tokenKind(ast->classkey_token) == T_UNION)
            head = "union ";
        tyName = head + extractId(ast->name->name);
    }

    writer_->writeTypedef(tyName, classTy);
    writer_->writeAnd(true);

    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    writer_->writeEquivRel(alpha, tyName);
    writer_->writeAnd(true);

    structs_.push(alpha);
    Scope *previousScope = switchScope(ast->symbol);
    for (DeclarationListAST *it = ast->member_specifier_list; it; it = it->next)
        visitDeclaration(it->value);
    switchScope(previousScope);
    structs_.pop();

    return false;
}

    //--- Statements

void ConstraintGenerator::visitStatement(StatementAST *ast)
{
    accept(ast);
}

bool ConstraintGenerator::visit(CompoundStatementAST *ast)
{
    DEBUG_VISIT(CompoundStatementAST);
    OBSERVE(CompoundStatementAST);

    Scope *previousScope = switchScope(ast->symbol);
    for (StatementListAST *it = ast->statement_list; it; it = it->next)
        visitStatement(it->value);
    switchScope(previousScope);

    return false;
}

bool ConstraintGenerator::visit(DeclarationStatementAST *ast)
{
    DEBUG_VISIT(DeclarationStatementAST);
    OBSERVE(DeclarationStatementAST);

    maybeFollowStmt();
    visitDeclaration(ast->declaration);
    seenStmt_ = false; // Actually a declaration (unset the stmt seen flag).

    return false;
}

bool ConstraintGenerator::visit(ExpressionStatementAST *ast)
{
    DEBUG_VISIT(ExpressionStatementAST);
    OBSERVE(ExpressionStatementAST);

    if (!ast->expression)
        return false;

    CLASSIFY(ast->expression);
    maybeFollowStmt();

    // Deal with an assignment.
    if (ast->expression->asBinaryExpression()) {
        switch (tokenKind(ast->expression->asBinaryExpression()->binary_op_token)) {
        case T_EQUAL:
        case T_PLUS_EQUAL:
        case T_MINUS_EQUAL:
        case T_STAR_EQUAL:
        case T_SLASH_EQUAL:
        case T_PERCENT_EQUAL:
        case T_LESS_LESS_EQUAL:
        case T_GREATER_GREATER_EQUAL:
        case T_CARET_EQUAL:
        case T_AMPER_EQUAL:
        case T_PIPE_EQUAL:
            visitExpression(ast->expression);
            return false;

        default:
            break;
        }
    }

    // Deal with a pointer dereference.
    if (ast->expression->asUnaryExpression()
            && (tokenKind(ast->expression->asUnaryExpression()->unary_op_token)
            == T_STAR)) {
        std::tuple<std::string, std::string> a1a2 = supply_.createTypeVar2();
        writer_->writeExists(std::get<0>(a1a2));
        writer_->writeExists(std::get<1>(a1a2));
        collectExpression(std::get<0>(a1a2), ast->expression->asUnaryExpression()->expression);
        writer_->writeAnd();
        writer_->writePtrRel(std::get<0>(a1a2), std::get<1>(a1a2));
        return false;
    }

    const std::string& alpha = supply_.createTypeVar1();
    writer_->writeExists(alpha);
    collectExpression(alpha, ast->expression);

    // In the case of a function call, we try to find out what's the actual
    // return type because, even though the function declaration may be absent,
    // the function definition may be present in later in the code. In such
    // case we use the return type for functions we know about.
    if (ast->expression->asCall()
            && ast->expression->asCall()->base_expression->asIdExpression()) {
        const std::string& funcName =
                trivialName(ast->expression->asCall()->base_expression->asIdExpression());
        const auto it = knownFuncNames_.find(funcName);
        if (it != knownFuncNames_.end()) {
            writer_->writeAnd();
            writer_->writeEquivRel(alpha, it->second);
        } else {
            knownFuncRets_[funcName].push_back(alpha);
        }
    }

    return false;
}

bool ConstraintGenerator::visit(IfStatementAST *ast)
{
    DEBUG_VISIT(IfStatementAST);
    OBSERVE(IfStatementAST);
    CLASSIFY(ast->condition);

    maybeFollowStmt();
    convertBoolExpression(ast->condition);

    if (ast->statement)
        visitStatement(ast->statement);

    if (ast->else_statement)
        visitStatement(ast->else_statement);

    return false;
}

bool ConstraintGenerator::visit(ReturnStatementAST *ast)
{
    DEBUG_VISIT(ReturnStatementAST);
    OBSERVE(ReturnStatementAST);

    if (ast->expression) {
        maybeFollowStmt();
        visitExpression(ast->expression);

        ENSURE_NONEMPTY_ALPHA_RET_STACK(return false);
        valuedRets_.top() = true;
    }

    return false;
}

bool ConstraintGenerator::visit(SwitchStatementAST *ast)
{
    DEBUG_VISIT(SwitchStatementAST);
    OBSERVE(SwitchStatementAST);

    maybeFollowStmt();
    convertBoolExpression(ast->condition);

    if (ast->statement)
        visitStatement(ast->statement);

    return false;
}

bool ConstraintGenerator::visit(CaseStatementAST *ast)
{
    DEBUG_VISIT(CaseStatementAST);
    OBSERVE(CaseStatementAST);

    maybeFollowStmt();
    collectExpression(kDefaultIntTy, ast->expression);
    if (ast->expression->asIdExpression()) {
        writer_->writeAnd();
        writer_->writeReadOnly(extractId(ast->expression->asIdExpression()->name->name));
    }

    if (ast->statement)
        visitStatement(ast->statement);

    return false;
}

bool ConstraintGenerator::visit(DoStatementAST *ast)
{
    DEBUG_VISIT(DoStatementAST);
    OBSERVE(DoStatementAST);

    maybeFollowStmt();
    convertBoolExpression(ast->expression);

    if (ast->statement)
        visitStatement(ast->statement);

    return false;
}

bool ConstraintGenerator::visit(WhileStatementAST *ast)
{
    DEBUG_VISIT(WhileStatementAST);
    OBSERVE(WhileStatementAST);

    maybeFollowStmt();
    convertBoolExpression(ast->condition);

    if (ast->statement)
        visitStatement(ast->statement);

    return false;
}

void ConstraintGenerator::maybeFollowStmt()
{
    if (seenStmt_)
        writer_->writeAnd(true);
    seenStmt_ = true;
}

bool ConstraintGenerator::visit(ForStatementAST *ast)
{
    DEBUG_VISIT(ForStatementAST);
    OBSERVE(ForStatementAST);

    // Declaration within a for statement in only available C99 onwards.
    if (ast->initializer
            && ((ast->initializer->asExpressionStatement()
                 && ast->initializer->asExpressionStatement()->expression)
                || ast->initializer->asDeclarationStatement())) {
        visitStatement(ast->initializer);
    }

    if (ast->condition) {
        maybeFollowStmt();
        convertBoolExpression(ast->condition);
    }

    if (ast->expression) {
        maybeFollowStmt();
        const std::string& alpha = supply_.createTypeVar1();
        writer_->writeExists(alpha);
        collectExpression(alpha, ast->expression);
    }

    if (ast->statement)
        visitStatement(ast->statement);

    if (ast->initializer->asDeclarationStatement()) {
        maybeFollowStmt();
        visitDeclaration(ast->initializer->asDeclarationStatement()->declaration);
        if (!preprocess_) {
            PSYCHE_ASSERT(!pendingEquivs_.empty(), return false, "expected for's declaration");
            const EquivPair& equiv = pendingEquivs_.top();
            writer_->writeEquivRel(equiv.first, equiv.second);
            pendingEquivs_.pop();
        }
    }

    return false;
}
