-module(template_gen_server_crash).
-behaviour(gen_server).

-export([start_link/1, test_call/2, test_cast/2, test_call/3, test_info/2, test/0]).

-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-define(SERVER,	?MODULE).

-record(state, {arg}).

%%  test callback has bug, will crash?
%%
%%CALLER
%%
test() ->
	{ok, Pid} = start_link([abc, efg]),
	test_call(Pid, good),
	test_call(Pid, error),
	test_cast(Pid, error).

start_link(Args) ->
	gen_server:start_link({local, ?SERVER}, ?MODULE, [Args], []).

test_call(Name, Call) ->
	gen_server:call(Name, Call).

test_call(Name, Call, Timeout) ->
	gen_server:call(Name, Call, Timeout).

test_cast(Name, Cast) ->
	gen_server:cast(Name, Cast).

test_info(Name, Info) ->
	Name ! {Info}.

%%
%% CALLBACK
%%

init([Args]) ->
	io:format("Args is ~p~n", [Args]),
	{ok, #state{arg=Args}}.
%	{ok, #state{arg=Args}, 10000}.

handle_call(error, {From,_}, State) ->
	io:format("call ~p ~p~n", [error, From]),
	2 = 1,
	{reply, call_ok, State};

handle_call(Msg, {From,_}, State) ->
	Reply = Msg,
	io:format("call ~p ~p~n", [Msg, From]),
	{reply, call_ok, State}.

handle_cast(error, State) ->
	io:format("cast ~p~n", [error]),
	2=1,
	{noreply, State};

handle_cast(Msg, State) ->
	io:format("cast ~p~n", [Msg]),
	{noreply, State}.

handle_info(Info, State) ->
	io:format("info ~p~n", [Info]),
	{stop, normal, State}.

terminate(Reason, _State) ->
	io:format("stop ~p~n", [Reason]),
	ok.

code_change(_OldVsn, _State, _Extra) ->
	{ok, _State}.

