#include <cmath>
#include "utils.hpp"
#include "osmlayer.hpp"

#define ONE_WEEK                (7*24*60*60)
#define MAX_TILE_BACKLOG        (150)
#define TILE_W                  (256)
#define TILE_H                  (256)

osmlayer::osmlayer(
        const std::string& nm,  
        const std::string& url, 
        unsigned int zmin,
        unsigned int zmax,
        unsigned int parallel,
        int imgtype) :
    layer(),                        // Base class constructor 
    m_canvas_0(500, 500),           // Canvas for 'double buffering'. Will be resized as needed
    m_canvas_1(500, 500),           // Canvas for 'double buffering'. Will be resized as needed
    m_canvas_tmp(500,500),          // Temporary drawing canvas. Will be resized as needed
    m_shutdown(false),              // Shutdown flag
    m_name(nm),                     // Layer name
    m_url(url),                     // Tileserver URL
    m_zmin(zmin),                   // Min. zoomlevel supported by server
    m_zmax(zmax),                   // Max. zoomlevel supported by server
    m_parallel(parallel),           // Number of simultaneous downloads
    m_type(imgtype)                 // Tile image data type    
{
    // Set map layer name
    name(m_name);

    // Create cache
    m_cache = new sqlitecache(
            settings::get_instance()["cache"]["location"].as<std::string>());

    // Create a cache session for the given URL
    m_cache->sessionid(url);

    // Create the requested number of download threads
    for (unsigned int i = 0; i<parallel; i++)
    {
        downloader *d = new downloader(new tileinfo());
        d->add_event_listener(this);
        m_downloaders.push_back(d);
    }

    // Register event handlers
    register_event_handler<osmlayer, downloader::event_complete>(this, &osmlayer::evt_downloadcomplete);
};

osmlayer::~osmlayer()
{
    // No new downloads will be started
    m_shutdown = true;

    // Clear the download queue, only active downloads may finish now
    m_downloadq.clear();

    // Wait for all pending downloads to be processed. download_process() will
    // usually only be called from the UI thread through FL::awake.
    // Unfortunately the UI thread is now stuck right here, waiting for our
    // destruction. So we need to poll the status of each remaining download.
#if 0
    bool all_idle;
    do {
        all_idle = true;

        std::vector<downloader*>::iterator it;
        for (it = m_downloaders.begin(); it != m_downloaders.end();++it)
        {
            if ((*it)->idle() == false)
            {
                all_idle = false;
                Fl::wait();
                break;
            }
        }
    } while (all_idle == false);
#endif

    m_test = true;
    Fl::awake(testcb, this);
    do 
    {
        Fl::wait();
    } while (m_test == true);

    std::cout << "all downloaders exited" << std::endl;

    // Destroy all downloaders
    std::vector<downloader*>::iterator it;
    for (it = m_downloaders.begin(); it != m_downloaders.end();++it)
    {
        delete static_cast<tileinfo*>((*it)->userdata());
        delete (*it);
    }

    // Destroy the cache
    delete m_cache;
};

void osmlayer::testcb(void *ud)
{
    std::cout << "testcb" << std::endl;
    static_cast<osmlayer*>(ud)->m_test = false;
}

bool osmlayer::evt_downloadcomplete(const downloader::event_complete *e)
{
    time_t expires = e->expires();
    time_t now = time(NULL);

    tileinfo *ti = static_cast<tileinfo*>(e->userdata());

    if (expires <= now)
        expires = now + ONE_WEEK;

    if (e->buf().size() != 0)
    {
        m_cache->put(ti->z, ti->x, ti->y, expires, e->buf());
        notify_observers();
    }

    std::cout << "download complete event" << std::endl;

    // Maybe we can start another download
    //download_startnext();

    return true;
}

void osmlayer::download_startnext(void)
{
    if (m_shutdown)
        return;

    // See whether there are more requests in the queue
    if (m_downloadq.size() == 0)
        return;

    // Find an idle dowloader
    std::vector<downloader*>::iterator it;
    for (it = m_downloaders.begin(); it != m_downloaders.end();++it)
    {
        if  ((*it)->idle())
            break;
    }

    // No idle downloader available right now
    if (it == m_downloaders.end())
        return;

    // Get the request from the queue and start it...
    tileinfo next = m_downloadq.back();
    m_downloadq.pop_back();

    //...if it is not already in the cache
    if (m_cache->exists(next.z, next.x, next.y) == sqlitecache::FOUND)
        return;


    std::cout << "downloading using " << (*it) << std::endl;

    // Consruct the url
    std::ostringstream sz, sx, sy;
    sz << next.z;
    sx << next.x;
    sy << next.y;

    std::string url(m_url);
    url.replace (url.find("$FLORBZ$"), std::string("$FLORBZ$").length(), sz.str());
    url.replace (url.find("$FLORBX$"), std::string("$FLORBX$").length(), sx.str());
    url.replace (url.find("$FLORBY$"), std::string("$FLORBY$").length(), sy.str());

    // Create and start the download
    *(static_cast<tileinfo*>((*it)->userdata())) = next;
    (*it)->fetch(url);
}

void osmlayer::download_qtile(const tileinfo& tile)
{
    // Queue is full, erase oldest entry
    if (m_downloadq.size() >= MAX_TILE_BACKLOG)
        m_downloadq.erase(m_downloadq.begin());

    // Add tile request to queue
    m_downloadq.push_back(tile);

    // Eventually start the next download
    download_startnext();
}

void osmlayer::draw(const viewport &vp, canvas &os)
{
    // Zoomlevel not supported by this tile layer
    if ((vp.z() < m_zmin) || (vp.z() > m_zmax))
    {
        // Simple white background. We might need to do something a little more
        // sophisticated in the future.
        m_canvas_0.resize(vp.w(), vp.h());
        m_canvas_0.fgcolor(color(255,255,255));
        m_canvas_0.fillrect(0, 0, (int)vp.w(), (int)vp.h());
        os.draw(m_canvas_0, 0, 0, (int)vp.w(), (int)vp.h(), 0, 0);

        // Make sure the map is redrawn on zoomlevel change by invalidating the
        // current local viewport
        m_vp.w(0);
        m_vp.h(0);

        return;
    }

    // Regular tile drawig
    update_map(vp);
    os.draw(m_canvas_0, 0, 0, (int)vp.w(), (int)vp.h(), 0, 0);
}

void osmlayer::update_map(const viewport &vp)
{
   // the current layer view is dirty if it is incomplete (tiles are missing or
   // outdated)
   static bool dirty = true;

   // Make sure the new viewport is any different from the last one before
   // going through the hassle of drawing the map anew.
   if ((m_vp != vp) || (dirty))
   {
     // Check if the old and new viewport intersect and eventually recycle any
     // portion of the map image
     viewport vp_inters(vp);
     vp_inters.intersect(m_vp);

     // Old and new viewport intersect, recycle old canvas map image buffer
     if (((vp_inters.w() > 0) && (vp_inters.h() > 0)) && (!dirty))
     {
       // Resize the drawing buffer if it is too small
       if ((m_canvas_1.w() < vp.w()) || (m_canvas_1.h() < vp.h()))
           m_canvas_1.resize(vp.w(), vp.h());

       m_canvas_1.draw(
               m_canvas_0, 
               vp_inters.x() - m_vp.x(), 
               vp_inters.y() - m_vp.y(), 
               vp_inters.w(), 
               vp_inters.h(), 
               vp_inters.x() - vp.x(), 
               vp_inters.y() - vp.y());

       // Draw any portion of the map image not covered by the old image
       // buffer. Effectively, the rectangles left, right, above and below the
       // intersection are drawn.
       for (int i=0;i<4;i++) 
       {
         unsigned long x_tmp, y_tmp, w_tmp, h_tmp;

         // Determine the respective rectangle coordinates
         switch (i)
         {
           // Rectangle to the left if there is one
           case 0:
             if (vp_inters.x() <= vp.x())
             {
               continue;
             }
             x_tmp = vp.x();
             y_tmp = vp.y();
             w_tmp = vp_inters.x() - vp.x();
             h_tmp = vp.h();
             break;
           // Rectangle to the right if there is one
           case 1:
             if ((vp_inters.x() + vp_inters.w()) >= (vp.x() + vp.w()))
             {
               continue;
             }
             x_tmp = vp_inters.x() + vp_inters.w();
             y_tmp = vp.y();
             w_tmp = (vp.x() + vp.w()) - (vp_inters.x() + vp_inters.w());
             h_tmp = vp.h();
             break;
           // Rectangle above if there is one
           case 2:
             if (vp_inters.y() <= vp.y())
             {
               continue;
             }
             x_tmp = vp_inters.x();
             y_tmp = vp.y();
             w_tmp = vp_inters.w();
             h_tmp = vp_inters.y() - vp.y();
             break;
           // Rectangle below if there is one
           case 3:
             if ((vp_inters.y() + vp_inters.h()) >= (vp.y() + vp.h()))
             {
               continue;
             }
             x_tmp = vp_inters.x();
             y_tmp = vp_inters.y() + vp_inters.h();
             w_tmp = vp_inters.w();
             h_tmp = (vp.y() + vp.h()) - (vp_inters.y() + vp_inters.h());
             break;
           default:
             ;
         }

         // Construct a temporary viewport for the respective rectangle
         viewport vp_tmp(x_tmp, y_tmp, vp.z(), w_tmp, h_tmp);

         // Resize the drawing buffer if it is too small
         if ((m_canvas_tmp.w() < vp_tmp.w()) || (m_canvas_tmp.h() < vp_tmp.h()))
             m_canvas_tmp.resize(vp_tmp.w(), vp_tmp.h());

         // Draw the rectangle
         if (!drawvp(vp_tmp, m_canvas_tmp))
            dirty = true;

         // Copy the dummy canvas image into the new map buffer
         m_canvas_1.draw(
            m_canvas_tmp, 
            0, 0, 
            vp_tmp.w(), vp_tmp.h(), 
            vp_tmp.x() - vp.x(), vp_tmp.y() - vp.y());
       }

       // Make the new canvas buffer the current one. We can't just copy
       // construct a temporary canvas object because it would free the
       // internal buffer when it goes out of scope. So we have to switch all
       // members manually here. 
       canvas_storage buftmp = m_canvas_0.buf();
       unsigned int wtmp = m_canvas_0.w();
       unsigned int htmp = m_canvas_0.h();

       m_canvas_0.buf(m_canvas_1.buf());
       m_canvas_0.w(m_canvas_1.w());
       m_canvas_0.h(m_canvas_1.h());

       m_canvas_1.buf(buftmp);
       m_canvas_1.w(wtmp);
       m_canvas_1.h(htmp);
     }
     else
     {
       // Draw the entire map area and make the resulting canvas buffer the
       // current one, it will have the required size (viewport size).
       if ((m_canvas_0.w() < vp.w()) || (m_canvas_0.h() < vp.h()))
           m_canvas_0.resize(vp.w(), vp.h());

       dirty = !drawvp(vp, m_canvas_0);
     }

     /* Save the new viewport for later reference */
     m_vp = vp;
   }
}

bool osmlayer::drawvp(const viewport &vp, canvas &c)
{
    // Return whether the map images has tiles missing (false) or not (true)
    bool ret = true;

    // Get the x and y start tile index
    unsigned int tstartx = vp.x() / TILE_W;
    unsigned int tstarty = vp.y() / TILE_H;
    
    // If the vp does not exactly hit the tile edge, there is an offset
    // for drawing the individual tiles.
    int pstartx = -((int)(vp.x() % TILE_W));
    int pstarty = -((int)(vp.y() % TILE_H));

    // Start at offset and draw up to vp.w() and vp.h()
    int px, py;
    unsigned int tx, ty;

    for (py=pstarty, ty=tstarty; py<(int)vp.h(); py+=TILE_W, ty++)
    {
       for (px=pstartx, tx=tstartx; px<(int)vp.w(); px+=TILE_H, tx++)
       {
          // Get the tile
          int rc = m_cache->get(vp.z(), tx, ty, m_imgbuf);
          
          // Draw the tile if we either have a valid or expired version of it...
          if (rc != sqlitecache::NOTFOUND)
          {
              image img(m_type, (unsigned char*)(&m_imgbuf[0]), m_imgbuf.size());
              c.draw(img, px, py);
          }
          // ...otherwise draw placeholder image
          else
          {
              c.fgcolor(color(200, 113, 113));
              c.fillrect(px, py, TILE_W, TILE_H);
          }

          // Tile not in cache or expired
          if ((rc == sqlitecache::EXPIRED) || 
              (rc == sqlitecache::NOTFOUND))
          {
              tileinfo t;
              t.z = vp.z();
              t.x = tx; 
              t.y = ty;
              download_qtile(t);
              ret = false;
          }
       }
    }

    return ret;
}
