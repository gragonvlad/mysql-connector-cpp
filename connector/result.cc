#include <mysql/cdk.h>
#include <mysqlxx.h>
#include <vector>
#include <sstream>
#include <iomanip>

/*
  Implementation of Result and Row interfaces.
*/

using namespace ::mysqlx;
using mysqlx::GUID;
using mysqlx::col_count_t;
using mysqlx::row_count_t;

using std::endl;

class bytes::Access
{
public:
  static bytes mk(byte *beg, size_t len) { return bytes(beg, beg+len); }
  static bytes mk(const cdk::bytes &data) { return bytes(data.begin(), data.end()); }
};

/*
  Convenience wrapper around std container that is used
  to store incoming raw bytes sequence.
*/

class Buffer
{
  std::vector<byte> m_impl;

public:

  void append(bytes data)
  {
    m_impl.insert(m_impl.end(), data.begin(), data.end());
  }

  size_t size() const { return m_impl.size(); }

  cdk::bytes data() const
  {
    return cdk::bytes((byte*)m_impl.data(), m_impl.size());
  }
};



/*
  Result implementation
  =====================
  This implementation stores all rows received from server in
  m_rows member. Done like this because cdk::Cursor::get_row() is
  not yet implemented.
*/

class Result::Impl
  : public Row
  , public cdk::Row_processor
{
  typedef std::map<col_count_t,Buffer> Row_data;

  cdk::Reply  *m_reply;
  cdk::Cursor *m_cursor;
  std::map<row_count_t,Row_data> m_rows;
  row_count_t  m_pos;
  GUID         m_guid;

  Impl(cdk::Reply *r)
    : m_reply(r)
    , m_cursor(NULL)
  {
    init();
  }

  Impl(cdk::Reply *r, const GUID &guid)
    : m_reply(r), m_cursor(NULL), m_guid(guid)
  {
    init();
  }

  void init()
  {
    if (!m_reply)
      return;

    m_reply->wait();

    if (m_reply->entry_count() > 0)
      return;

    if (m_reply->has_results())
    {
      m_cursor= new cdk::Cursor(*m_reply);
      m_cursor->wait();
      // TODO: do it asynchronously
      read_rows();
      m_cursor->close();
      // Note: we keep cursor instance because it gives access to meta-data
    }
  }

  ~Impl()
  {
    delete m_reply;
    delete m_cursor;
  }

  /*
    Read all rows and store raw data in m_rows.
  */

  void read_rows();

  /*
    This class instance works as Row instance too. The
    m_pos member tells which row is being accessed.
  */

  Row* get_row(row_count_t pos)
  {
    if (!m_cursor)
      return NULL;
    if (pos >= m_rows.size())
      return NULL;
    m_pos= pos;
    return this;
  }

  // Row

  const mysqlx::string getString(mysqlx::col_count_t pos);
  mysqlx::bytes getBytes(mysqlx::col_count_t pos);

  // Row_processor

  bool row_begin(row_count_t pos)
  {
    m_pos= pos;
    m_rows.insert(std::pair<row_count_t,Row_data>(pos, Row_data()));
    return true;
  }
  void row_end(row_count_t) {}

  size_t field_begin(col_count_t pos, size_t);
  void   field_end(col_count_t) {}
  void   field_null(col_count_t) {}
  size_t field_data(col_count_t pos, bytes);
  void   end_of_data() {}

  friend class Row_builder;
  friend class Result;
  friend class RowResult;
};


bytes Result::Impl::getBytes(mysqlx::col_count_t pos)
{
  return mysqlx::bytes::Access::mk(m_rows.at(m_pos).at(pos).data());
}

/*
  Get value stored in column `pos` in row `m_pos` as a string.

  For debugging purposes no real conversion to native C++ types
  is done, but instead the returned string has format:

    "<type>: <bytes>"

  where <type> is the type of the column as reported by CDK and <bytes>
  is a hex dump of the value. For STRING values we assume they are ascii
  strings and show as stings, NULL values are shown as "<null>".
*/

const mysqlx::string Result::Impl::getString(mysqlx::col_count_t pos)
{
  if (!m_cursor)
    throw "empty row";
  if (pos > m_cursor->col_count())
    throw "pos out of range";

  std::wstringstream out;

  // Add prefix with name of the CDK column type

#define TYPE_NAME(X)  case cdk::TYPE_##X: out <<#X <<": "; break;

  switch (m_cursor->type(pos))
  {
    CDK_TYPE_LIST(TYPE_NAME)
  default:
    out <<"UNKNOWN(" <<m_cursor->type(pos) <<"): "; break;
  }

  // Append value bytes

  Row_data &rd= m_rows.at(m_pos);

  try {
    // will throw out_of_range exception if column at `pos` is NULL
    cdk::bytes data= rd.at(pos).data();

    switch (m_cursor->type(pos))
    {
    case cdk::TYPE_STRING:
      // assume ascii string
      out <<'"' <<std::string(data.begin(), data.end()-1).c_str() <<'"';
      break;

    default:

      // Output other values as sequences of hex digits

      for(byte *ptr= data.begin(); ptr < data.end(); ++ptr)
      {
        out <<std::setw(2) <<std::setfill(L'0') <<std::hex <<(unsigned)*ptr;
      };
      break;
    }
  }
  catch (std::out_of_range&)
  {
    out <<"<null>";
  }

  return out.str();
}


void Result::Impl::read_rows()
{
  if (!m_cursor)
    return;
  m_cursor->get_rows(*this);
  m_cursor->wait();
}



size_t Result::Impl::field_begin(col_count_t pos, size_t)
{
  Row_data &rd= m_rows[m_pos];
  rd.insert(std::pair<col_count_t,Buffer>(pos, Buffer()));
  return 1024;
}

size_t Result::Impl::field_data(col_count_t pos, bytes data)
{
  m_rows[(unsigned)m_pos][(unsigned)pos].append(mysqlx::bytes::Access::mk(data));
  return 1024;
}


/*
  Result
  ======
*/


Result::Result(cdk::Reply *r)
{
  m_impl= new Impl(r);
}

Result::Result(cdk::Reply *r, const GUID &guid)
{
  m_impl= new Impl(r,guid);
}


Result::~Result()
{
  if (m_owns_impl)
    delete m_impl;
}


const GUID& Result::getLastDocumentId() const
{
  if (!m_impl)
    throw "Empty result";
  return m_impl->m_guid;
}


/*
  RowResult
  =========
*/


Row* RowResult::next()
{
  return m_impl->get_row(m_pos++);
}


col_count_t RowResult::getColumnCount() const
{
  if (!m_impl)
    throw "Empty result";
  if (!m_impl->m_cursor)
    throw "No result set";
  return m_impl->m_cursor->col_count();
}


/*
  DocResult
  =========
*/


class JSON_printer
  : public cdk::JSON::Processor
{
  ostream &m_out;
  unsigned m_indent;
  cdk::string m_op_name;

public:

  JSON_printer(ostream &out, unsigned ind =0)
    : m_out(out), m_indent(ind)
  {}

  ostream& out_ind()
  {
    for (unsigned i=0; i < 2*m_indent; ++i)
      m_out.put(' ');
    return m_out;
  }

  ostream& out_key(const cdk::string &key)
  {
    return out_ind() <<key <<": ";
  }

  // JSON processor

  void doc_begin()
  {
    out_ind() <<"{" <<endl;
    m_indent++;
  }

  void doc_end()
  {
    m_indent--;
    out_ind() <<"}" <<endl;
  }

  void key_doc(const cdk::string &key, const Document &val)
  {
    out_key(key) <<"<sub-document>" <<endl;
    m_indent++;
    val.process(*this);
    m_indent--;
  }

  void key_val(const cdk::string &key, const Value &val)
  {
    out_key(key);
    val.process(*this);
    m_out <<endl;
  }

  void str(const cdk::string &val) { m_out <<val; }
  void num(uint64_t val) { m_out <<val; }
  void num(int64_t val) { m_out <<val; }
  void num(float val) { m_out <<val; }
  void num(double val) { m_out <<val; }
  void yesno(bool val) { m_out <<(val ? "true" : "false"); }

};


class DocResult::Impl
  : public DbDoc
  , RowResult
{
  Row    *m_row;
  bool   m_at_front;

  Impl(Result &init)
    : RowResult(std::move(init)), m_at_front(true)
  {
    m_row= next();
  }

  void next_row()
  {
    if (m_at_front)
      m_at_front= false;
    else
      m_row= next();
  }

  // DbDoc

  void print(ostream &out) const
  {
    if (!m_row)
      throw "No documents";
    bytes data= m_row->getBytes(0);
    cdk::Codec<cdk::TYPE_DOCUMENT> codec;
    JSON_printer dp(out);
    codec.from_bytes(cdk::bytes(data.begin(), data.end()-1), dp);
  }

  friend class DocResult;
};


void DocResult::operator=(Result &&init)
{
  Result::operator=(std::move(init));
  delete m_doc_impl;
  m_doc_impl= new Impl(init);
}

DocResult::~DocResult()
{
  delete m_doc_impl;
}


DbDoc& DocResult::first()
{
  return *m_doc_impl;
}

DbDoc* DocResult::next()
{
  m_doc_impl->next_row();
  return m_doc_impl->m_row ? m_doc_impl : NULL;
}

