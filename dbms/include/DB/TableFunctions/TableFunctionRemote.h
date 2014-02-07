#pragma once

#include <DB/TableFunctions/ITableFunction.h>
#include <DB/Storages/StorageDistributed.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/DataStreams/RemoteBlockInputStream.h>

struct data;
namespace DB
{

/*
 * remote('address', db, table) - создаёт временный StorageDistributed.
 * Чтобы получить структуру таблицы, делается запрос DESC TABLE на удалённый сервер.
 * Например:
 * SELECT count() FROM remote('example01-01-1', merge, hits) - пойти на example01-01-1, в БД merge, таблицу hits. *
 */

/// Пока не реализована.
class TableFunctionRemote: public ITableFunction
{
public:
	/// Максимальное количество различных шардов и максимальное количество реплик одного шарда
	const size_t MAX_ADDRESSES = 200;

	std::string getName() const { return "remote"; }

	StoragePtr execute(ASTPtr ast_function, Context & context)
	{

		/** В запросе в качестве аргумента для движка указано имя конфигурационной секции,
		  *  в которой задан список удалённых серверов, а также имя удалённой БД и имя удалённой таблицы.
		  */
		ASTs & args_func = dynamic_cast<ASTFunction &>(*ast_function).children;

		if (args_func.size() != 1)
			throw Exception("Storage Distributed requires 3 parameters"
				" - name of configuration section with list of remote servers, name of remote database, name of remote table.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;

		if (args.size() != 3)
			throw Exception("Storage Distributed requires 3 parameters"
				" - description of remote servers, name of remote database, name of remote table.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		String descripton	 	= safeGet<const String &>(dynamic_cast<ASTLiteral &>(*args[0]).value);
		String remote_database 	= dynamic_cast<ASTIdentifier &>(*args[1]).name;
		String remote_table 	= dynamic_cast<ASTIdentifier &>(*args[2]).name;

		/// В InterpreterSelectQuery будет создан ExpressionAnalzyer, который при обработке запроса наткнется на эти Identifier.
		/// Нам необходимо их пометить как имя базы данных и таблицы посколку по умолчанию стоит значение column
		dynamic_cast<ASTIdentifier &>(*args[1]).kind = ASTIdentifier::Database;
		dynamic_cast<ASTIdentifier &>(*args[2]).kind = ASTIdentifier::Table;

		std::vector <std::vector< String> > names;
		std::vector<String> shards = parseDescription(descripton, 0, descripton.size(), ',');
		for (size_t i = 0; i < shards.size(); ++i)
			names.push_back(parseDescription(shards[i], 0, shards[i].size(), '|'));

		cluster = new Cluster(context.getSettings(), context.getDataTypeFactory(), names);

		return StorageDistributed::create(getName(), chooseColumns(*cluster, remote_database, remote_table, context), remote_database,
										  remote_table, *cluster, context.getDataTypeFactory(), context.getSettings(), context);
	}

private:
	Poco::SharedPtr<Cluster> cluster; /// Ссылка на объект кластер передается в StorageDistributed и должен существовать до выполнения запроса

	/// Узнать имена и типы столбцов для создания таблицы
	NamesAndTypesListPtr chooseColumns(Cluster & cluster, const String & database, const String &table, const Context & context) const
	{
		/// Запрос на описание таблицы
		String query = "DESC TABLE " + database + "." + table;
		Settings settings = context.getSettings();
		/// Отправляем на первый попавшийся сервер
		detail::ConnectionPoolEntry entry = (*cluster.pools.begin())->get(&settings);

		NamesAndTypesList res;
		/// Парсим результат запроса и формируем NamesAndTypesList
		{
			BlockInputStreamPtr input = new RemoteBlockInputStream(entry, query, &settings, QueryProcessingStage::Complete);
			input->readPrefix();
			while (true)
			{
				Block current = input->read();
				if (!current)
					break;
				ColumnPtr name = current.getByName("name").column;
				ColumnPtr type = current.getByName("type").column;
				size_t size = name->size();
				for (size_t i = 0; i < size; ++i)
				{
					Field column, data_type;
					name->get(i, column);
					type->get(i, data_type);
					String column_name = column.get<String>();
					String data_type_name = data_type.get<String>();
					std::cerr << column_name << " " << data_type_name << std::endl;
					res.push_back(std::make_pair(column_name, context.getDataTypeFactory().get(data_type_name)));
				}
			}
		}
		return new NamesAndTypesList(res);
	}

	/// Декартово произведение двух множеств строк, результат записываем на место первого аргумента
	void append(std::vector<String> & to, const std::vector<String> & what) const
	{
		if (what.empty()) return;
		if (to.empty())
		{
			to = what;
			return;
		}
		if (what.size() * to.size() > MAX_ADDRESSES)
			throw Exception("Storage Distributed, first argument generates too many result addresses",
							ErrorCodes::BAD_ARGUMENTS);
		std::vector<String> res;
		for (size_t i = 0; i < to.size(); ++i)
			for (size_t j = 0; j < what.size(); ++j)
				res.push_back(to[i] + what[j]);
		to.swap(res);
	}

	/// Парсим число из подстроки
	bool parseId(const String & description, size_t l, size_t r, size_t & res) const
	{
		res = 0;
		for (size_t pos = l; pos < r; pos ++)
		{
			if (!isdigit(description[pos]))
				return false;
			res = res * 10 + description[pos] - '0';
			if (res > 1e15)
				return false;
		}
		return true;
	}

	/* Парсит строку, генерирующую шарды и реплики. Spliter - один из двух символов | или '
	 * в зависимости от того генерируются шарды или реплики.
	 * Например:
	 * host1,host2,... - порождает множество шардов из host1, host2, ...
	 * host1|host2|... - порождает множество реплик из host1, host2, ...
	 * abc{8..10}def - порождает множество шардов abc8def, abc9def, abc10def.
	 * abc{08..10}def - порождает множество шардов abc08def, abc09def, abc10def.
	 * abc{x,yy,z}def - порождает множество шардов abcxdef, abcyydef, abczdef.
	 * abc{x|yy|z}def - порождает множество реплик abcxdef, abcyydef, abczdef.
	 * abc{1..9}de{f,g,h} - прямое произведение, 27 шардов.
	 * abc{1..9}de{0|1} - прямое произведение, 9 шардов, в каждом 2 реплики.
	 */
	std::vector<String> parseDescription(const String & description, size_t l, size_t r, char spliter) const
	{
		std::vector<String> res;
		std::vector<String> cur;

		/// Пустая подстрока, означает множество из пустой строки
		if (l >= r)
		{
			res.push_back("");
			return res;
		}

		for (size_t i = l; i < r; ++i)
		{
			/// Либо числовой интервал (8..10) либо аналогичное выражение в скобках
			if (description[i] == '{')
			{
				int cnt = 1;
				int last_dot = -1; /// Самая правая пара точек, запоминаем индекс правой из двух
				size_t m;
				std::vector<String> buffer;
				bool have_spliter = false;

				/// Ищем соответствующую нашей закрывающую скобку
				for (m = i + 1; m < r; ++m)
				{
					if (description[m] == '{') ++cnt;
					if (description[m] == '}') --cnt;
					if (description[m] == '.' && description[m-1] == '.') last_dot = m;
					if (description[m] == spliter) have_spliter = true;
					if (cnt == 0) break;
				}
				if (cnt != 0)
					throw Exception("Storage Distributed, incorrect brace sequence in first argument",
									ErrorCodes::BAD_ARGUMENTS);
				/// Наличие точки означает, что числовой интервал
				if (last_dot != -1)
				{
					size_t left, right;
					if (description[last_dot - 1] != '.')
						throw Exception("Storage Distributed, incorrect argument in braces (only one dot): " + description.substr(i, m - i + 1),
										ErrorCodes::BAD_ARGUMENTS);
					if (!parseId(description, i + 1, last_dot - 1, left))
						throw Exception("Storage Distributed, incorrect argument in braces (Incorrect left number): "
										+ description.substr(i, m - i + 1),
										ErrorCodes::BAD_ARGUMENTS);
					if (!parseId(description, last_dot + 1, m, right))
						throw Exception("Storage Distributed, incorrect argument in braces (Incorrect right number): "
										+ description.substr(i, m - i + 1),
										ErrorCodes::BAD_ARGUMENTS);
					if (left > right)
						throw Exception("Storage Distributed, incorrect argument in braces (left number is greater then right): "
										+ description.substr(i, m - i + 1),
										ErrorCodes::BAD_ARGUMENTS);
					if (right - left + 1 >  MAX_ADDRESSES)
						throw Exception("Storage Distributed, first argument generates too many result addresses",
							ErrorCodes::BAD_ARGUMENTS);
					for (size_t id = left; id <= right; ++id)
						buffer.push_back(toString<uint64>(id));
				} else if (have_spliter) /// Если внутри есть текущий разделитель, то сгенерировать множество получаемых строк
					buffer = parseDescription(description, i + 1, m, spliter);
				else 					/// Иначе просто скопировать, порождение произойдет при вызове с правильным разделителем
					buffer.push_back(description.substr(i, m - i + 1));
				/// К текущему множеству строк добавить все возможные полученные продолжения
				append(cur, buffer);
				i = m;
			} else if (description[i] == spliter) {
				/// Если разделитель, то добавляем в ответ найденные строки
				res.insert(res.end(), cur.begin(), cur.end());
				cur.clear();
			} else {
				/// Иначе просто дописываем символ к текущим строкам
				std::vector<String> buffer;
				buffer.push_back(description.substr(i, 1));
				append(cur, buffer);
			}
		}
		res.insert(res.end(), cur.begin(), cur.end());
		if (res.size() > MAX_ADDRESSES)
			throw Exception("Storage Distributed, first argument generates too many result addresses",
							ErrorCodes::BAD_ARGUMENTS);
		return res;
	}

};

}
