/*
 * Copyright 2011, Oliver Tappe <zooey@hirschkaefer.de>
 * Copyright 2016, Andrew Lindesay <apl@lindesay.co.nz>
 * Distributed under the terms of the MIT License.
 */
#ifndef PACKAGE_INFO_PARSER_H
#define PACKAGE_INFO_PARSER_H


#include <package/PackageInfo.h>


namespace BPackageKit {


/*
 * Parses a ".PackageInfo" file and fills a BPackageInfo object with the
 * package info elements found.
 */
class BPackageInfo::Parser {
public:
								Parser(ParseErrorListener* listener = NULL);

			status_t			Parse(const BString& packageInfoString,
									BPackageInfo* packageInfo);

			status_t			ParseVersion(const BString& versionString,
									bool revisionIsOptional,
									BPackageVersion& _version);
			status_t			ParseResolvable(
									const BString& expressionString,
									BPackageResolvable& _expression);
			status_t			ParseResolvableExpression(
									const BString& expressionString,
									BPackageResolvableExpression& _expression);

private:
			struct UrlStringValidator;
			struct StringValidator;
			struct ParseError;
			struct Token;
			struct ListElementParser;
	friend	struct ListElementParser;

			enum TokenType {
				TOKEN_STRING,
				TOKEN_OPERATOR_ASSIGN,
				TOKEN_OPERATOR_LESS,
				TOKEN_OPERATOR_LESS_EQUAL,
				TOKEN_OPERATOR_EQUAL,
				TOKEN_OPERATOR_NOT_EQUAL,
				TOKEN_OPERATOR_GREATER_EQUAL,
				TOKEN_OPERATOR_GREATER,
				TOKEN_OPEN_BRACE,
				TOKEN_CLOSE_BRACE,
				TOKEN_ITEM_SEPARATOR,
				//
				TOKEN_EOF,
			};

private:
			Token				_NextToken();
			void				_RewindTo(const Token& token);

			void				_ParseStringValue(BString* value,
									const char** _tokenPos = NULL);
			uint32				_ParseFlags();
			void				_ParseArchitectureValue(
									BPackageArchitecture* value);
			void				_ParseVersionValue(BPackageVersion* value,
									bool revisionIsOptional);
	static	void				_ParseVersionValue(Token& word,
									BPackageVersion* value,
									bool revisionIsOptional);
			void				_ParseResolvable(
									const Token& token,
									BPackageResolvable& _value);
			void				_ParseResolvableExpression(
									const Token& token,
									BPackageResolvableExpression& _value,
									BString* _basePackage);
			void				_ParseList(ListElementParser& elementParser,
									bool allowSingleNonListElement);
			void				_ParseStringList(BStringList* value,
									bool requireResolvableName = false,
									bool convertToLowerCase = false,
									StringValidator* stringValidator = NULL);
			void				_ParseResolvableList(
									BObjectList<BPackageResolvable, true>* value);
			void				_ParseResolvableExprList(
									BObjectList<BPackageResolvableExpression, true>*
										value,
									BString* _basePackage = NULL);
			void				_ParseGlobalWritableFileInfos(
									GlobalWritableFileInfoList* infos);
			void				_ParseUserSettingsFileInfos(
									UserSettingsFileInfoList* infos);
			void				_ParseUsers(UserList* users);

			void				_Parse(BPackageInfo* packageInfo);

	static	bool				_IsAlphaNumUnderscore(const BString& string,
									const char* additionalChars,
									int32* _errorPos);
	static	bool				_IsAlphaNumUnderscore(const char* string,
									const char* additionalChars,
									int32* _errorPos);
	static	bool				_IsAlphaNumUnderscore(const char* start,
									const char* end,
									const char* additionalChars,
									int32* _errorPos);
	static	bool				_IsValidResolvableName(const char* string,
									int32* _errorPos);

private:
			ParseErrorListener*	fListener;
			const char*			fPos;
};


struct BPackageInfo::Parser::ParseError {
	BString 	message;
	const char*	pos;

	ParseError(const BString& _message, const char* _pos)
		: message(_message), pos(_pos)
	{
	}
};


struct BPackageInfo::Parser::Token {
	TokenType	type;
	BString		text;
	const char*	pos;

	Token(TokenType _type, const char* _pos, int length = 0,
		const char* text = NULL)
		:
		type(_type),
		pos(_pos)
	{
		if (text != NULL)
			this->text = text;
		else if (length != 0)
			this->text.SetTo(pos, length);
	}

	operator bool() const
	{
		return type != TOKEN_EOF;
	}
};


struct BPackageInfo::Parser::ListElementParser {
	virtual ~ListElementParser()
	{
	}

	virtual void operator()(const Token& token) = 0;
};


struct BPackageInfo::Parser::StringValidator {
public:
	virtual void Validate(const BString &string, const char *pos) = 0;
};


struct BPackageInfo::Parser::UrlStringValidator
    : public BPackageInfo::Parser::StringValidator {
public:
	virtual	void				Validate(const BString &string, const char* pos);
};


} // namespace BPackageKit


#endif	// PACKAGE_INFO_PARSER_H
