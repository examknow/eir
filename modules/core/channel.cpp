#include "eir.h"
#include "handler.h"

#include <functional>

#include <paludis/util/tokeniser.hh>

#include <paludis/util/wrapped_forward_iterator-impl.hh>

using namespace eir;

struct ChannelHandler : CommandHandlerBase<ChannelHandler>, Module
{
    void handle_join(const Message *);
    void handle_part(const Message *);
    void handle_quit(const Message *);
    void handle_names_reply(const Message *);
    void handle_nick(const Message *);
    void handle_kick(const Message *);
    void handle_account(const Message *);
    void handle_who_reply(const Message *);
    void handle_whox_reply(const Message *);

    ChannelHandler();

    CommandHolder join_id, part_id, quit_id, names_id, nick_id, account_id, who_id, whox_id, kick_id;
};

ChannelHandler::ChannelHandler()
{
    join_id = add_handler(filter_command_type("JOIN", sourceinfo::RawIrc), &ChannelHandler::handle_join);
    part_id = add_handler(filter_command_type("PART", sourceinfo::RawIrc), &ChannelHandler::handle_part);
    quit_id = add_handler(filter_command_type("QUIT", sourceinfo::RawIrc), &ChannelHandler::handle_quit);
    //names_id = add_handler("353", sourceinfo::RawIrc, &ChannelHandler::handle_names_reply);
    nick_id = add_handler(filter_command_type("NICK", sourceinfo::RawIrc), &ChannelHandler::handle_nick);
    account_id = add_handler(filter_command_type("ACCOUNT", sourceinfo::RawIrc), &ChannelHandler::handle_account);
    who_id = add_handler(filter_command_type("352", sourceinfo::RawIrc), &ChannelHandler::handle_who_reply);
    whox_id = add_handler(filter_command_type("354", sourceinfo::RawIrc), &ChannelHandler::handle_whox_reply);
    kick_id = add_handler(filter_command_type("KICK", sourceinfo::RawIrc), &ChannelHandler::handle_kick);
}

namespace
{
    Client::ptr find_or_create_client(Bot *b, std::string nick, std::string user, std::string host)
    {
        Client::ptr c = b->find_client(nick);

        if(!c)
        {
            c.reset(new Client(b, nick, user, host));
            b->add_client(c);
        }

        return c;
    }

    Client::ptr find_or_create_client(Bot *b, std::string name, std::string nuh)
    {
        Client::ptr c = b->find_client(name);

        if(!c)
        {
            // We don't know anything about this client. Make a new client struct and join it.
            std::string nick, user, host;
            std::string::size_type bang, at;
            bang = nuh.find('!');
            if(bang == std::string::npos)
            {
                // We don't know this client's user@host yet. Leave it blank.
                nick = nuh;
                user = "";
                host = "";
            }
            else
            {
                nick = nuh.substr(0, bang);
                at = nuh.find('@', bang + 1);
                user = nuh.substr(bang + 1, at - bang - 1);
                host = nuh.substr(at + 1, std::string::npos);
            }

            c.reset(new Client(b, nick, user, host));
            b->add_client(c);
        }

        return c;
    }

    Client::ptr find_or_create_client(const Message *m)
    {
        Client::ptr c = m->source.client;
        if (c)
            return c;

        return find_or_create_client(m->bot, m->source.name, m->source.raw);
    }

    Channel::ptr find_or_create_channel(Bot *b, std::string name)
    {
        Channel::ptr ch = b->find_channel(name);

        if (!ch)
        {
            ch.reset(new Channel(name));
            b->add_channel(ch);
        }
        return ch;
    }

    Channel::ptr find_or_create_channel(const Message *m)
    {
        return find_or_create_channel(m->bot, m->source.destination);
    }

    void client_leaving_channel(Bot *b, Client::ptr c, Channel::ptr ch)
    {
        // If we don't know anything about the client, then they can't be in our channel lists.
        if (!c)
            return;

        if(!ch)
            return;

        c->leave_chan(ch);

        if(c->begin_channels() == c->end_channels())
        {
            // If they don't share any channels, we can't know anything about them.
            b->remove_client(c);
        }

        if (c != b->me())
            return;

        // If we just left, we need to remove any knowledge of the channel.
        Channel::MemberIterator member = ch->begin_members();
        while (member != ch->end_members())
        {
            Membership::ptr p = *member++;
            p->client->leave_chan(p);
        }

        b->remove_channel(ch);
    }
}

void ChannelHandler::handle_join(const Message *m)
{
    Context ctx("Processing join for " + m->source.name + " to " + m->source.destination);

    Client::ptr c = find_or_create_client(m);
    Channel::ptr ch = find_or_create_channel(m);

    if (m->bot->use_account_tracking() && m->args.size() > 0)
        c->set_account(m->args[0]);

    c->join_chan(ch);

    if (m->source.name == m->bot->nick())
    {
        std::string who_command = "WHO " + m->source.destination;
        if (m->bot->use_account_tracking())
            who_command += " %cnuhaft,524";

        m->bot->send(who_command);
    }
}

void ChannelHandler::handle_names_reply(const Message *m)
{
    if (m->args.size() < 2)
        return;

    std::string chname = m->args[1];
    std::vector<std::string> nicks;

    Context ctx("Processing NAMES reply for " + chname);

    paludis::tokenise_whitespace(m->args[2], std::back_inserter(nicks));

    Channel::ptr ch = find_or_create_channel(m->bot, chname);

    for (std::vector<std::string>::iterator it = nicks.begin(), ite = nicks.end();
            it != ite; ++it)
    {
        Client::ptr c = find_or_create_client(m->bot, *it, *it);
        c->join_chan(ch);
    }
}

static void who_reply_common(const Message *m,
                             std::string chname, std::string nick, std::string user, std::string hostname,
                             std::string flags, std::string account)
{
    Context ctx("Processing WHO reply for " + chname + " (" + nick + ")");
    Client::ptr c = find_or_create_client(m->bot, nick, user, hostname);

    if (m->bot->use_account_tracking() && !account.empty())
        c->set_account(account);

    Channel::ptr ch = find_or_create_channel(m->bot, chname);
    Membership::ptr member = c->join_chan(ch);

    for (std::string::iterator ch = flags.begin(); ch != flags.end(); ++ch)
    {
        char c = m->bot->supported()->get_prefix_mode(*ch);
        if (c && !member->has_mode(c))
            member->modes += c;
    }
}

void ChannelHandler::handle_who_reply(const Message *m)
{
    if (m->args.size() != 7) return;

    std::string chname = m->args[0],
                user = m->args[1],
                hostname = m->args[2],
                /* server = m->args[3], */
                nick = m->args[4],
                flags = m->args[5];

    who_reply_common(m, chname, nick, user, hostname, flags, "*");
}

void ChannelHandler::handle_whox_reply(const Message *m)
{
    // Check that this was a reply from the same type of WHOX request that we sent on join
    if (m->args.size() != 7)
        return;
    if (m->args[0] != "524")
        return;

    std::string chname = m->args[1],
                user = m->args[2],
                host = m->args[3],
                nick = m->args[4],
                flags = m->args[5],
                account = m->args[6];

    // WHOX uses "0" for "no account", unlike account-notify which uses *
    if (account == "0")
        account = "";

    who_reply_common(m, chname, nick, user, host, flags, account);
}

void ChannelHandler::handle_part(const Message *m)
{
    Context ctx("Processing part for " + m->source.name + " from " + m->source.destination);

    Client::ptr c = m->source.client;
    Bot *b = m->bot;

    Channel::ptr ch = b->find_channel(m->source.destination);

    client_leaving_channel(b, c, ch);
}

void ChannelHandler::handle_kick(const Message *m)
{
    if (m->args.empty())
        return;

    Context ctx("Processing kick for " + m->args[0] + " from " + m->source.destination);

    Bot *b = m->bot;

    Client::ptr c = b->find_client(m->args[0]);
    Channel::ptr ch = b->find_channel(m->source.destination);

    client_leaving_channel(b, c, ch);
}

void ChannelHandler::handle_quit(const Message *m)
{
    Context ctx("Handling quit from " + m->source.name);

    Client::ptr c = m->source.client;
    Bot *b = m->bot;

    if (!c)
        return;

    for (Client::ChannelIterator chi = c->begin_channels(), che = c->end_channels();
            chi != che; )
        c->leave_chan(*chi++);

    b->remove_client(c);

    Logger::get_instance()->Log(b, c, Logger::Debug, "QUIT: " + c->nick());
}

void ChannelHandler::handle_nick(const Message *m)
{
    Context ctx("Handling nick change from " + m->source.name);

    if(!m->source.client)
        return;
    std::string newnick(m->source.destination);
    m->source.client->change_nick(newnick);
}

void ChannelHandler::handle_account(const Message *m)
{
    Context ctx("Handling account change from " + m->source.name);

    if (!m->source.client)
        return;

    std::string newaccount(m->source.destination);
    m->source.client->set_account(newaccount);
}

MODULE_CLASS(ChannelHandler)
