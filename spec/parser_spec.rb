require "spec_helper"
require "json"

describe HTTP::Parser do
  before(:each) do
    @parser = HTTP::Parser.new

    @headers = nil
    @body = ""
    @encoding = 'utf-8'
    @started = false
    @done = false

    @parser.on_message_begin = proc{ @started = true }
    @parser.on_headers_complete = proc { |e| @headers = e }
    @parser.on_body = proc { |chunk|
      @body << chunk
    }
    @parser.on_message_complete = proc{ @done = true }
  end

  it "should implement basic api" do
    if defined? Encoding
      orig_internal_enc = Encoding.default_internal
      Encoding.default_internal = nil
      @rb_encoding = Encoding.find(@encoding)
      @body.encode!(@rb_encoding)
    end
    @parser <<
      "GET /test?ok=1 HTTP/1.1\r\n" +
      "User-Agent: curl/7.18.0\r\n" +
      "Host: 0.0.0.0:5000\r\n" +
      "Accept: */*\r\n" +
      "Content-Length: 5\r\n" +
      "Content-Type: text/plain; encoding=#{@encoding}\r\n" +
      "\r\n" +
      "World"

    @started.should be_true
    @done.should be_true

    @parser.http_major.should == 1
    @parser.http_minor.should == 1
    @parser.http_version.should == [1,1]
    @parser.http_method.should == 'GET'
    @parser.status_code.should be_nil

    @parser.request_url.should == '/test?ok=1'
    @parser.request_path.should == '/test'
    @parser.query_string.should == 'ok=1'
    @parser.fragment.should be_empty

    @parser.headers.should == @headers
    @parser.headers['User-Agent'].should == 'curl/7.18.0'
    @parser.headers['Host'].should == '0.0.0.0:5000'

    @body.should == "World"
    if defined? Encoding
      @body.encoding.should eql(@rb_encoding)
      Encoding.default_internal = orig_internal_enc
    end
  end

  if defined? Encoding
    it "should implement basic api respecting Encoding.default_internal" do
      orig_internal_enc = Encoding.default_internal
      enc = 'Windows-1256'
      rb_enc = Encoding.find enc
      Encoding.default_internal = rb_enc
      @body.encode!(rb_enc)

      @parser <<
        "GET /test?ok=1 HTTP/1.1\r\n" +
        "User-Agent: curl/7.18.0\r\n" +
        "Host: 0.0.0.0:5000\r\n" +
        "Accept: */*\r\n" +
        "Content-Length: 5\r\n" +
        "Content-Type: text/plain; encoding=#{@encoding}\r\n" +
        "\r\n" +
        "World"

      @started.should be_true
      @done.should be_true

      @parser.http_major.should == 1
      @parser.http_minor.should == 1
      @parser.http_version.should == [1,1]
      @parser.http_method.should == 'GET'
      @parser.http_method.encoding.should eql(rb_enc)
      @parser.status_code.should be_nil

      @parser.request_url.should == '/test?ok=1'
      @parser.request_url.encoding.should eql(rb_enc)
      @parser.request_path.should == '/test'
      @parser.request_path.encoding.should eql(rb_enc)
      @parser.query_string.should == 'ok=1'
      @parser.query_string.encoding.should eql(rb_enc)
      @parser.fragment.should be_empty

      @parser.headers.should == @headers
      @parser.headers.keys.each do |key|
        key.encoding.should eql(rb_enc)
      end
      @parser.headers['User-Agent'].should == 'curl/7.18.0'
      @parser.headers['User-Agent'].encoding.should eql(rb_enc)
      @parser.headers['Host'].should == '0.0.0.0:5000'
      @parser.headers['Host'].encoding.should eql(rb_enc)

      @body.should == "World"
      @body.encoding.should eql(rb_enc)
      Encoding.default_internal = orig_internal_enc
    end
  end

  %w[ request response ].each do |type|
    JSON.parse(File.read(File.expand_path("../support/#{type}s.json", __FILE__))).each do |test|
      test['headers'] ||= {}

      it "should parse #{type}: #{test['name']}" do
        @parser << test['raw']

        @parser.keep_alive?.should == test['should_keep_alive'] unless RUBY_PLATFORM =~ /java/
        @parser.upgrade?.should == (test['upgrade']==1)
        @parser.http_method.should == test['method']

        fields = %w[
          http_major
          http_minor
        ]

        if test['type'] == 'HTTP_REQUEST'
          fields += %w[
            request_url
            request_path
            query_string
            fragment
          ]
        else
          fields += %w[
            status_code
          ]
        end

        fields.each do |field|
          @parser.send(field).should == test[field]
        end

        @headers.size.should == test['num_headers']
        @headers.should == test['headers']

        @body.should == test['body']
        @body.size.should == test['body_size'] if test['body_size']
      end
    end
  end
end
