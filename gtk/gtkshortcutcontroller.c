/*
 * Copyright © 2018 Benjamin Otte
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Benjamin Otte <otte@gnome.org>
 */


/**
 * SECTION:gtkshortcutcontroller
 * @Short_description: Event controller for shortcuts
 * @Title: GtkShortcutController
 * @See_also: #GtkEventController, #GtkShortcut
 *
 * #GtkShortcutController is an event controller that manages shortcuts.
 **/

#include "config.h"

#include "gtkshortcutcontrollerprivate.h"

#include "gtkeventcontrollerprivate.h"
#include "gtkintl.h"
#include "gtkshortcut.h"
#include "gtkshortcutmanager.h"
#include "gtkshortcuttrigger.h"
#include "gtktypebuiltins.h"
#include "gtkwidgetprivate.h"

#include <gdk/gdk.h>

struct _GtkShortcutController
{
  GtkEventController parent_instance;

  GSList *shortcuts;
  GtkShortcutScope scope;
  GdkModifierType mnemonics_modifiers;

  guint run_class : 1;
  guint run_managed : 1;
};

struct _GtkShortcutControllerClass
{
  GtkEventControllerClass parent_class;
};

enum {
  PROP_0,
  PROP_MNEMONICS_MODIFIERS,
  PROP_SCOPE,

  N_PROPS
};

static GParamSpec *properties[N_PROPS] = { NULL, };

G_DEFINE_TYPE (GtkShortcutController, gtk_shortcut_controller,
               GTK_TYPE_EVENT_CONTROLLER)

static gboolean
gtk_shortcut_controller_is_rooted (GtkShortcutController *self)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self));

  if (widget == NULL)
    return FALSE;

  return gtk_widget_get_root (widget) != NULL;
}

static void
gtk_shortcut_controller_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_MNEMONICS_MODIFIERS:
      gtk_shortcut_controller_set_mnemonics_modifiers (self, g_value_get_flags (value));
      break;

    case PROP_SCOPE:
      gtk_shortcut_controller_set_scope (self, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_shortcut_controller_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_MNEMONICS_MODIFIERS:
      g_value_set_flags (value, self->mnemonics_modifiers);
      break;

    case PROP_SCOPE:
      g_value_set_enum (value, self->scope);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_shortcut_controller_dispose (GObject *object)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (object);

  g_slist_free_full (self->shortcuts, g_object_unref);
  self->shortcuts = NULL;

  G_OBJECT_CLASS (gtk_shortcut_controller_parent_class)->dispose (object);
}

static gboolean
gtk_shortcut_controller_trigger_shortcut (GtkShortcutController *self,
                                          GtkShortcut           *shortcut,
                                          GdkEvent              *event,
                                          gboolean               enable_mnemonics)
{
  if (!gtk_shortcut_trigger_trigger (gtk_shortcut_get_trigger (shortcut), event, enable_mnemonics))
    return FALSE;

  return gtk_shortcut_action_activate (gtk_shortcut_get_action (shortcut),
                                       GTK_SHORTCUT_ACTION_EXCLUSIVE, /* FIXME */
                                       gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self)),
                                       gtk_shortcut_get_arguments (shortcut));
}

static gboolean
gtk_shortcut_controller_run_controllers (GtkEventController *controller,
                                         GdkEvent           *event,
                                         double              x,
                                         double              y,
                                         gboolean            enable_mnemonics)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (controller);
  GtkWidget *widget;
  const GSList *l;

  for (l = self->shortcuts; l; l = l->next)
    {
      if (gtk_shortcut_controller_trigger_shortcut (self, l->data, event, enable_mnemonics))
        return TRUE;
    }

  if (self->run_class)
    {
      widget = gtk_event_controller_get_widget (controller);

      for (l = gtk_widget_class_get_shortcuts (GTK_WIDGET_GET_CLASS (widget)); l; l = l->next)
        {
          if (gtk_shortcut_controller_trigger_shortcut (self, l->data, event, enable_mnemonics))
            return TRUE;
        }
    }

  if (self->run_managed)
    {
      GtkPropagationPhase current_phase = gtk_event_controller_get_propagation_phase (controller);
      widget = gtk_event_controller_get_widget (controller); 
      
      for (l = g_object_get_data (G_OBJECT (widget), "gtk-shortcut-controllers"); l; l = l->next)
        {
          if (gtk_event_controller_get_propagation_phase (l->data) != current_phase)
            continue;

          if (gtk_shortcut_controller_run_controllers (l->data, event, x, y, enable_mnemonics))
            return TRUE;
        }
    }

  return FALSE;
}

static gboolean
gtk_shortcut_controller_handle_event (GtkEventController *controller,
                                      GdkEvent           *event,
                                      double              x,
                                      double              y)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (controller);
  gboolean enable_mnemonics;

  if (self->scope != GTK_SHORTCUT_SCOPE_LOCAL)
    return FALSE;

  if (gdk_event_get_event_type (event) == GDK_KEY_PRESS)
    {
      GdkModifierType modifiers;
      modifiers = gdk_event_get_modifier_state (event);
      enable_mnemonics = (modifiers & gtk_accelerator_get_default_mod_mask ()) == self->mnemonics_modifiers;
    }
  else
    {
      enable_mnemonics = FALSE;
    }

  return gtk_shortcut_controller_run_controllers (controller, event, x, y, enable_mnemonics);
}

static void
gtk_shortcut_controller_set_widget (GtkEventController *controller,
                                    GtkWidget          *widget)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (controller);

  GTK_EVENT_CONTROLLER_CLASS (gtk_shortcut_controller_parent_class)->set_widget (controller, widget);

  if (_gtk_widget_get_root (widget))
    gtk_shortcut_controller_root (self);
}

static void
gtk_shortcut_controller_unset_widget (GtkEventController *controller)
{
  GtkShortcutController *self = GTK_SHORTCUT_CONTROLLER (controller);
  GtkWidget *widget = gtk_event_controller_get_widget (controller);

  if (_gtk_widget_get_root (widget))
    gtk_shortcut_controller_unroot (self);

  GTK_EVENT_CONTROLLER_CLASS (gtk_shortcut_controller_parent_class)->unset_widget (controller);
}

static void
gtk_shortcut_controller_class_init (GtkShortcutControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkEventControllerClass *controller_class = GTK_EVENT_CONTROLLER_CLASS (klass);

  object_class->dispose = gtk_shortcut_controller_dispose;
  object_class->set_property = gtk_shortcut_controller_set_property;
  object_class->get_property = gtk_shortcut_controller_get_property;

  controller_class->handle_event = gtk_shortcut_controller_handle_event;
  controller_class->set_widget = gtk_shortcut_controller_set_widget;
  controller_class->unset_widget = gtk_shortcut_controller_unset_widget;

  /**
   * GtkShortcutController:mnemonic-modifiers:
   *
   * The modifiers that need to be pressed to allow mnemonics activation.
   */
  properties[PROP_MNEMONICS_MODIFIERS] =
      g_param_spec_flags ("mnemonic-modifiers",
                          P_("Mnemonic modifers"),
                          P_("The modifiers to be pressed to allow mnemonics activation"),
                          GDK_TYPE_MODIFIER_TYPE,
                          GDK_MOD1_MASK,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GtkShortcutController:scope:
   *
   * What scope the shortcuts will be handled in.
   */
  properties[PROP_SCOPE] =
      g_param_spec_enum ("scope",
                         P_("Scope"),
                         P_("What scope the shortcuts will be handled in"),
                         GTK_TYPE_SHORTCUT_SCOPE,
                         GTK_SHORTCUT_SCOPE_LOCAL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gtk_shortcut_controller_init (GtkShortcutController *self)
{
  self->mnemonics_modifiers = GDK_MOD1_MASK;
}

void
gtk_shortcut_controller_root (GtkShortcutController *self)
{
  GtkShortcutManager *manager;

  switch (self->scope)
    {
    case GTK_SHORTCUT_SCOPE_LOCAL:
      return;

    case GTK_SHORTCUT_SCOPE_MANAGED:
      {
        GtkWidget *widget;

        for (widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self));
             !GTK_IS_SHORTCUT_MANAGER (widget);
             widget = _gtk_widget_get_parent (widget))
          ;

        if (!GTK_IS_SHORTCUT_MANAGER (widget))
          return;

        manager = GTK_SHORTCUT_MANAGER (widget);
      }
      break;

    case GTK_SHORTCUT_SCOPE_GLOBAL:
      {
        GtkRoot *root = gtk_widget_get_root (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self)));

        if (!GTK_IS_SHORTCUT_MANAGER (root))
          return;

        manager = GTK_SHORTCUT_MANAGER (root);
      }
      break;

    default:
      g_assert_not_reached ();
      return;
    }

  GTK_SHORTCUT_MANAGER_GET_IFACE (manager)->add_controller (manager, self);
}

void
gtk_shortcut_controller_unroot (GtkShortcutController *self)
{
  GtkShortcutManager *manager;

  switch (self->scope)
    {
    case GTK_SHORTCUT_SCOPE_LOCAL:
      return;

    case GTK_SHORTCUT_SCOPE_MANAGED:
      {
        GtkWidget *widget;

        for (widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self));
             !GTK_IS_SHORTCUT_MANAGER (widget);
             widget = _gtk_widget_get_parent (widget))
          ;

        if (!GTK_IS_SHORTCUT_MANAGER (widget))
          return;

        manager = GTK_SHORTCUT_MANAGER (widget);
      }
      break;

    case GTK_SHORTCUT_SCOPE_GLOBAL:
      {
        GtkRoot *root = gtk_widget_get_root (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (self)));

        if (!GTK_IS_SHORTCUT_MANAGER (root))
          return;

        manager = GTK_SHORTCUT_MANAGER (root);
      }
      break;

    default:
      g_assert_not_reached ();
      return;
    }

  GTK_SHORTCUT_MANAGER_GET_IFACE (manager)->remove_controller (manager, self);
}

GtkEventController *
gtk_shortcut_controller_new (void)
{
  return g_object_new (GTK_TYPE_SHORTCUT_CONTROLLER,
                       NULL);
}

void
gtk_shortcut_controller_set_run_class (GtkShortcutController  *controller,
                                       gboolean                run_class)
{
  controller->run_class = run_class;
}

void
gtk_shortcut_controller_set_run_managed (GtkShortcutController  *controller,
                                         gboolean                run_managed)
{
  controller->run_managed = run_managed;
}

/**
 * gtk_shortcut_controller_add_shortcut:
 * @self: the controller
 * @shortcut: a #GtkShortcut
 *
 * Adds @shortcut to the list of shortcuts handled by @self.
 *
 * The shortcut is added to the list so that it is triggered before
 * all existing shortcuts.
 *
 * FIXME: What's supposed to happen if a shortcut gets added twice?
 **/
void
gtk_shortcut_controller_add_shortcut (GtkShortcutController *self,
                                      GtkShortcut           *shortcut)
{
  g_return_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self));
  g_return_if_fail (GTK_IS_SHORTCUT (shortcut));

  g_object_ref (shortcut);
  self->shortcuts = g_slist_prepend (self->shortcuts, shortcut);
}

/**
 * gtk_shortcut_controller_remove_shortcut:
 * @self: the controller
 * @shortcut: a #GtkShortcut
 *
 * Removes @shortcut from the list of shortcuts handled by @self.
 *
 * If @shortcut had not been added to @controller, this function does
 * nothing.
 **/
void
gtk_shortcut_controller_remove_shortcut (GtkShortcutController  *self,
                                         GtkShortcut            *shortcut)
{
  GSList *l;

  g_return_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self));
  g_return_if_fail (GTK_IS_SHORTCUT (shortcut));

  l = g_slist_find (self->shortcuts, shortcut);
  if (l == NULL)
    return;

  self->shortcuts = g_slist_delete_link (self->shortcuts, l);
  g_object_unref (shortcut);
}

/**
 * gtk_shortcut_controller_set_scope:
 * @self: a #GtkShortcutController
 * @scope: the new scope to use
 *
 * Sets the controller to have the given @scope.
 *
 * The scope allows shortcuts to be activated outside of the normal
 * event propagation. In particular, it allows installing global
 * keyboard shortcuts that can be activated even when a widget does
 * not have focus.
 **/
void
gtk_shortcut_controller_set_scope (GtkShortcutController *self,
                                   GtkShortcutScope       scope)
{
  gboolean rooted;

  g_return_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self));

  if (self->scope == scope)
    return;

  rooted = gtk_shortcut_controller_is_rooted (self);

  if (rooted)
    gtk_shortcut_controller_unroot (self);

  self->scope = scope;

  if (rooted)
    gtk_shortcut_controller_root (self);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SCOPE]);
}

/**
 * gtk_shortcut_controller_get_scope:
 * @self: a #GtkShortcutController
 *
 * Gets the scope for when this controller activates its shortcuts. See
 * gtk_shortcut_controller_set_scope() for details.
 *
 * Returns: the controller's scope
 **/
GtkShortcutScope
gtk_shortcut_controller_get_scope (GtkShortcutController *self)
{
  g_return_val_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self), GTK_SHORTCUT_SCOPE_LOCAL);

  return self->scope;
}

/**
 * gtk_shortcut_controller_set_mnemonics_modifiers:
 * @self: a #GtkShortcutController
 * @modifiers: the new mnemonics_modifiers to use
 *
 * Sets the controller to have the given @mnemonics_modifiers.
 *
 * The mnemonics modifiers determines which modifiers need to be pressed to allow
 * activation of shortcuts with mnemonics triggers.
 *
 * This value is only relevant for local shortcut controllers. Global and managed
 * shortcut controllers will have their shortcuts activated from other places which
 * have their own modifiers for activating mnemonics.
 **/
void
gtk_shortcut_controller_set_mnemonics_modifiers (GtkShortcutController *self,
                                                 GdkModifierType        modifiers)
{
  g_return_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self));

  if (self->mnemonics_modifiers == modifiers)
    return;

  self->mnemonics_modifiers = modifiers;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MNEMONICS_MODIFIERS]);
}

/**
 * gtk_shortcut_controller_get_mnemonics_modifiers:
 * @self: a #GtkShortcutController
 *
 * Gets the mnemonics modifiers for when this controller activates its shortcuts. See
 * gtk_shortcut_controller_set_mnemonics_modifiers() for details.
 *
 * Returns: the controller's mnemonics modifiers
 **/
GdkModifierType
gtk_shortcut_controller_get_mnemonics_modifiers (GtkShortcutController *self)
{
  g_return_val_if_fail (GTK_IS_SHORTCUT_CONTROLLER (self), GTK_SHORTCUT_SCOPE_LOCAL);

  return self->mnemonics_modifiers;
}
